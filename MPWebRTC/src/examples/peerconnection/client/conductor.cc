/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "examples/peerconnection/client/conductor.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "absl/memory/memory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_parameters.h"
#include "api/rtp_sender_interface.h"
#include "api/scoped_refptr.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
#include "api/task_queue/task_queue_factory.h"
#include "api/test/create_frame_generator.h"
#include "api/transport/bitrate_settings.h"
#include "api/video/video_frame.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "examples/peerconnection/client/defaults.h"
#include "examples/peerconnection/client/main_wnd.h"
#include "examples/peerconnection/client/peer_connection_client.h"
#include "json/reader.h"
#include "json/value.h"
#include "json/writer.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/network_route_probe_bitrate_tracker.h"
#include "rtc_base/strings/json.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/clock.h"
#include "test/frame_generator_capturer.h"
#include "test/platform_video_capturer.h"
#include "test/test_video_capturer.h"

ABSL_DECLARE_FLAG(bool, record_test_metrics);
ABSL_DECLARE_FLAG(std::string, metrics_output_path);
ABSL_DECLARE_FLAG(int, metrics_duration_seconds);
ABSL_DECLARE_FLAG(int, metrics_interval_seconds);
ABSL_DECLARE_FLAG(int, video_jitter_buffer_min_delay_ms);

namespace {
using webrtc::test::TestVideoCapturer;

// Lab bitrate guardrails: allow native bandwidth adaptation inside this range.
constexpr int kLabMinBitrateBps = 2300000;    // 2.3 Mbps
constexpr int kLabStartBitrateBps = 2300000;  // 2.3 Mbps
constexpr int kLabMaxBitrateBps = 5000000;  // 5 Mbps

void ApplyVideoReceiverJitterBufferDelayFromFlag(
    webrtc::RtpReceiverInterface* receiver) {
  if (!receiver || receiver->media_type() != webrtc::MediaType::VIDEO) {
    return;
  }
  if (webrtc::GetWebrtcMultipathSendMode() !=
      webrtc::WebrtcMultipathSendMode::kExtended) {
    RTC_LOG(LS_INFO)
        << "Skip video jitter buffer minimum delay: only enabled in "
           "extended mode";
    return;
  }
  const int delay_ms = absl::GetFlag(FLAGS_video_jitter_buffer_min_delay_ms);
  if (delay_ms < 0) {
    RTC_LOG(LS_WARNING) << "Ignore negative --video_jitter_buffer_min_delay_ms="
                        << delay_ms;
    return;
  }
  receiver->SetJitterBufferMinimumDelay(delay_ms / 1000.0);
  RTC_LOG(LS_INFO) << "Applied video jitter buffer minimum delay: " << delay_ms
                   << " ms";
}

void ApplyLabSendBitrateDefaults(webrtc::PeerConnectionInterface* pc) {
  if (!pc) {
    return;
  }
  webrtc::BitrateSettings settings;
  settings.min_bitrate_bps = kLabMinBitrateBps;
  settings.start_bitrate_bps = kLabStartBitrateBps;
  settings.max_bitrate_bps = kLabMaxBitrateBps;
  webrtc::RTCError err = pc->SetBitrate(settings);
  if (!err.ok()) {
    RTC_LOG(LS_WARNING) << "SetBitrate(lab adaptive range) failed: "
                        << err.message();
  } else {
    RTC_LOG(LS_INFO) << "SetBitrate adaptive range: min=" << kLabMinBitrateBps
                     << " start=" << kLabStartBitrateBps
                     << " max=" << kLabMaxBitrateBps << " bps";
  }
}

void ApplyLabSenderNoResizeDefaults(webrtc::PeerConnectionInterface* pc) {
  if (!pc) {
    return;
  }
  for (const auto& sender : pc->GetSenders()) {
    if (!sender || !sender->track() ||
        sender->track()->kind() !=
            webrtc::MediaStreamTrackInterface::kVideoKind) {
      continue;
    }
    webrtc::RtpParameters params = sender->GetParameters();
    params.degradation_preference =
        webrtc::DegradationPreference::MAINTAIN_RESOLUTION;
    webrtc::RTCError err = sender->SetParameters(params);
    if (!err.ok()) {
      RTC_LOG(LS_WARNING) << "Video sender SetParameters(no-resize) failed: "
                          << err.message();
    } else {
      RTC_LOG(LS_INFO) << "Applied sender params: degradation=MAINTAIN_RESOLUTION";
    }
  }
}

// Names used for a IceCandidate JSON object.
const char kCandidateSdpMidName[] = "sdpMid";
const char kCandidateSdpMlineIndexName[] = "sdpMLineIndex";
const char kCandidateSdpName[] = "candidate";

// Names used for a SessionDescription JSON object.
const char kSessionDescriptionTypeName[] = "type";
const char kSessionDescriptionSdpName[] = "sdp";

class DummySetSessionDescriptionObserver
    : public webrtc::SetSessionDescriptionObserver {
 public:
  static webrtc::scoped_refptr<DummySetSessionDescriptionObserver> Create() {
    return webrtc::make_ref_counted<DummySetSessionDescriptionObserver>();
  }
  void OnSuccess() override { RTC_LOG(LS_INFO) << __FUNCTION__; }
  void OnFailure(webrtc::RTCError error) override {
    RTC_LOG(LS_INFO) << __FUNCTION__ << " " << ToString(error.type()) << ": "
                     << error.message();
  }
};

std::unique_ptr<TestVideoCapturer> CreateCapturer(
    webrtc::TaskQueueFactory& task_queue_factory) {
  const size_t kWidth = 1280;
  const size_t kHeight = 960;
  const size_t kFps = 30;
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (!info) {
    return nullptr;
  }
  int num_devices = info->NumberOfDevices();
  for (int i = 0; i < num_devices; ++i) {
    std::unique_ptr<TestVideoCapturer> capturer =
        webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, i);
    if (capturer) {
      return capturer;
    }
  }
// [修改开始] 替换原有的 CreateSquareFrameGenerator

  // 1. 定义 YUV 文件路径 (建议使用绝对路径，避免找不到文件)
  std::vector<std::string> yuv_filenames = {
      "/home/chenyan/output_1280x960.yuv" // 替换为你实际的文件路径
  };

  // 2. 调用你提供的函数
  // frame_repeat_count = 1 表示每一帧在切到下一帧前只播放一次 (正常速度)
  auto frame_generator = webrtc::test::CreateFromYuvFileFrameGenerator(
      yuv_filenames, kWidth, kHeight, 1);
  return std::make_unique<webrtc::test::FrameGeneratorCapturer>(
      webrtc::Clock::GetRealTimeClock(), std::move(frame_generator), kFps,
      task_queue_factory);
}
class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static webrtc::scoped_refptr<CapturerTrackSource> Create(
      webrtc::TaskQueueFactory& task_queue_factory) {
    std::unique_ptr<TestVideoCapturer> capturer =
        CreateCapturer(task_queue_factory);
    if (capturer) {
      capturer->Start();
      return webrtc::make_ref_counted<CapturerTrackSource>(std::move(capturer));
    }
    return nullptr;
  }

 protected:
  explicit CapturerTrackSource(std::unique_ptr<TestVideoCapturer> capturer)
      : VideoTrackSource(/*remote=*/false), capturer_(std::move(capturer)) {}

 private:
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }

  std::unique_ptr<TestVideoCapturer> capturer_;
};

class PeriodicMetricsLogger : public webrtc::RTCStatsCollectorCallback {
 public:
  static constexpr double kExpectedPlaybackFps = 30.0;

  explicit PeriodicMetricsLogger(const std::string& output_path)
      : start_time_(std::chrono::steady_clock::now()),
        last_sample_time_(start_time_) {
    csv_.open(output_path, std::ios::out | std::ios::trunc);
    if (!csv_.is_open()) {
      RTC_LOG(LS_ERROR) << "Failed to open metrics output file: " << output_path;
      return;
    }
    csv_ << "elapsed_s,throughput_bps,fps,total_freeze_s,qp_avg\n";
    csv_.flush();
    RTC_LOG(LS_INFO) << "Metrics logging to: " << output_path;
  }

  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report)
      override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!csv_.is_open()) {
      return;
    }

    uint64_t total_video_bytes_received = 0;
    bool has_video_bytes = false;
    double fps_sum = 0.0;
    int fps_count = 0;
    double total_freeze_s = 0.0;
    uint64_t total_qp_sum = 0;
    uint64_t total_frames_decoded = 0;

    for (const auto& inbound :
         report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
      if (inbound->kind.has_value() && inbound->kind.value() != "video") {
        continue;
      }
      if (!inbound->kind.has_value() && !inbound->frames_decoded.has_value()) {
        continue;
      }
      if (inbound->bytes_received.has_value()) {
        total_video_bytes_received += inbound->bytes_received.value();
        has_video_bytes = true;
      }
      if (inbound->frames_per_second.has_value()) {
        fps_sum += inbound->frames_per_second.value();
        ++fps_count;
      }
      if (inbound->total_freezes_duration.has_value()) {
        total_freeze_s += inbound->total_freezes_duration.value();
      }
      if (inbound->qp_sum.has_value()) {
        total_qp_sum += inbound->qp_sum.value();
      }
      if (inbound->frames_decoded.has_value()) {
        total_frames_decoded += inbound->frames_decoded.value();
      }
    }

    const auto now = std::chrono::steady_clock::now();
    const double elapsed_s =
        std::chrono::duration<double>(now - start_time_).count();
    const double delta_s =
        std::chrono::duration<double>(now - last_sample_time_).count();
    last_sample_time_ = now;

    double throughput_bps = -1.0;
    if (has_video_bytes && prev_total_video_bytes_received_.has_value() &&
        delta_s > 0.0 &&
        total_video_bytes_received >= *prev_total_video_bytes_received_) {
      throughput_bps =
          static_cast<double>(total_video_bytes_received -
                              *prev_total_video_bytes_received_) *
          8.0 / delta_s;
    }
    if (has_video_bytes) {
      prev_total_video_bytes_received_ = total_video_bytes_received;
    }

    const double fps = (fps_count > 0) ? (fps_sum / fps_count) : -1.0;

    // Some environments keep `total_freezes_duration` at 0 even when playback
    // is visibly stalling due to heavy drops. Add a decoded-frame based stall
    // estimator as a fallback and report the larger cumulative stall time.
    if (prev_total_frames_decoded_.has_value() && delta_s > 0.0 &&
        total_frames_decoded >= *prev_total_frames_decoded_) {
      const uint64_t delta_frames_decoded =
          total_frames_decoded - *prev_total_frames_decoded_;
      const double expected_play_time_s =
          static_cast<double>(delta_frames_decoded) / kExpectedPlaybackFps;
      const double estimated_stall_s =
          std::max(0.0, delta_s - expected_play_time_s);
      synthetic_total_stall_s_ += estimated_stall_s;
    }
    const double reported_total_stall_s =
        std::max(total_freeze_s, synthetic_total_stall_s_);

    double qp_avg = -1.0;
    if (prev_total_qp_sum_.has_value() && prev_total_frames_decoded_.has_value() &&
        total_qp_sum >= *prev_total_qp_sum_ &&
        total_frames_decoded >= *prev_total_frames_decoded_) {
      const uint64_t delta_qp = total_qp_sum - *prev_total_qp_sum_;
      const uint64_t delta_frames =
          total_frames_decoded - *prev_total_frames_decoded_;
      if (delta_frames > 0) {
        qp_avg = static_cast<double>(delta_qp) / delta_frames;
      }
    }
    prev_total_qp_sum_ = total_qp_sum;
    prev_total_frames_decoded_ = total_frames_decoded;

    csv_ << std::fixed << std::setprecision(3) << elapsed_s << ",";
    if (throughput_bps >= 0.0) {
      csv_ << static_cast<uint64_t>(throughput_bps);
    } else {
      csv_ << -1;
    }
    csv_ << ",";
    if (fps >= 0.0) {
      csv_ << std::setprecision(2) << fps;
    } else {
      csv_ << -1;
    }
    csv_ << "," << std::setprecision(3) << reported_total_stall_s << ",";
    if (qp_avg >= 0.0) {
      csv_ << std::setprecision(3) << qp_avg;
    } else {
      csv_ << -1;
    }
    csv_ << "\n";
    csv_.flush();
  }

 private:
  std::ofstream csv_;
  std::mutex mutex_;
  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point last_sample_time_;
  std::optional<uint64_t> prev_total_video_bytes_received_;
  std::optional<uint64_t> prev_total_qp_sum_;
  std::optional<uint64_t> prev_total_frames_decoded_;
  double synthetic_total_stall_s_ = 0.0;
};

}  // namespace

Conductor::Conductor(const webrtc::Environment& env,
                     PeerConnectionClient* absl_nonnull client,
                     MainWindow* absl_nonnull main_wnd)
    : peer_id_(-1),
      loopback_(false),
      env_(env),
      client_(client),
      main_wnd_(main_wnd) {
  client_->RegisterObserver(this);
  main_wnd->RegisterObserver(this);
}

Conductor::~Conductor() {
  StopMetricsLogging();
  RTC_DCHECK(!peer_connection_);
}

bool Conductor::connection_active() const {
  return peer_connection_ != nullptr;
}

void Conductor::Close() {
  client_->SignOut();
  DeletePeerConnection();
}

bool Conductor::InitializePeerConnection() {
  RTC_DCHECK(!peer_connection_factory_);
  RTC_DCHECK(!peer_connection_);

  if (!signaling_thread_.get()) {
    signaling_thread_ = webrtc::Thread::CreateWithSocketServer();
    signaling_thread_->Start();
  }

  webrtc::PeerConnectionFactoryDependencies deps;
  deps.signaling_thread = signaling_thread_.get();
  deps.env = env_,
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  deps.video_encoder_factory =
      std::make_unique<webrtc::VideoEncoderFactoryTemplate<
          webrtc::LibvpxVp8EncoderTemplateAdapter,
          webrtc::LibvpxVp9EncoderTemplateAdapter,
          webrtc::OpenH264EncoderTemplateAdapter,
          webrtc::LibaomAv1EncoderTemplateAdapter>>();
  deps.video_decoder_factory =
      std::make_unique<webrtc::VideoDecoderFactoryTemplate<
          webrtc::LibvpxVp8DecoderTemplateAdapter,
          webrtc::LibvpxVp9DecoderTemplateAdapter,
          webrtc::OpenH264DecoderTemplateAdapter,
          webrtc::Dav1dDecoderTemplateAdapter>>();
  webrtc::EnableMedia(deps);
  peer_connection_factory_ =
      webrtc::CreateModularPeerConnectionFactory(std::move(deps));

  if (!peer_connection_factory_) {
    main_wnd_->MessageBox("Error", "Failed to initialize PeerConnectionFactory",
                          true);
    DeletePeerConnection();
    return false;
  }

  if (!CreatePeerConnection()) {
    main_wnd_->MessageBox("Error", "CreatePeerConnection failed", true);
    DeletePeerConnection();
  }

  AddTracks();

  return peer_connection_ != nullptr;
}

bool Conductor::ReinitializePeerConnectionForLoopback() {
  loopback_ = true;
  std::vector<webrtc::scoped_refptr<webrtc::RtpSenderInterface>> senders =
      peer_connection_->GetSenders();
  peer_connection_ = nullptr;
  // Loopback is only possible if encryption is disabled.
  webrtc::PeerConnectionFactoryInterface::Options options;
  options.disable_encryption = true;
  peer_connection_factory_->SetOptions(options);
  if (CreatePeerConnection()) {
    for (const auto& sender : senders) {
      peer_connection_->AddTrack(sender->track(), sender->stream_ids());
    }
    ApplyLabSenderNoResizeDefaults(peer_connection_.get());
    peer_connection_->CreateOffer(
        this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  }
  options.disable_encryption = false;
  peer_connection_factory_->SetOptions(options);
  return peer_connection_ != nullptr;
}

bool Conductor::CreatePeerConnection() {
  RTC_DCHECK(peer_connection_factory_);
  RTC_DCHECK(!peer_connection_);

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  // Lab fixed-quality experiments: disable CPU-driven adaptation so sender
  // does not downscale resolution when load/network fluctuates.
  config.set_cpu_adaptation(false);
  config.set_suspend_below_min_bitrate(false);
  config.media_config.video.enable_send_packet_batching = true;
  webrtc::PeerConnectionInterface::IceServer server;
  server.uri = GetPeerConnectionString();
  config.servers.push_back(server);

  webrtc::PeerConnectionDependencies pc_dependencies(this);
  auto error_or_peer_connection =
      peer_connection_factory_->CreatePeerConnectionOrError(
          config, std::move(pc_dependencies));
  if (error_or_peer_connection.ok()) {
    peer_connection_ = std::move(error_or_peer_connection.value());
    ApplyLabSendBitrateDefaults(peer_connection_.get());
  }
  return peer_connection_ != nullptr;
}

void Conductor::DeletePeerConnection() {
  StopMetricsLogging();
  main_wnd_->StopLocalRenderer();
  main_wnd_->StopRemoteRenderer();
  peer_connection_ = nullptr;
  peer_connection_factory_ = nullptr;
  peer_id_ = -1;
  loopback_ = false;
}

void Conductor::EnsureStreamingUI() {
  RTC_DCHECK(peer_connection_);
  if (main_wnd_->IsWindow()) {
    if (main_wnd_->current_ui() != MainWindow::STREAMING)
      main_wnd_->SwitchToStreamingUI();
  }
}

//
// PeerConnectionObserver implementation.
//

void Conductor::OnAddTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&
        streams) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << receiver->id();
  ApplyVideoReceiverJitterBufferDelayFromFlag(receiver.get());
  if (receiver->track() &&
      receiver->track()->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
    StartMetricsLoggingIfNeeded();
  }
  main_wnd_->QueueUIThreadCallback(NEW_TRACK_ADDED,
                                   receiver->track().release());
}

void Conductor::OnRemoveTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << receiver->id();
  main_wnd_->QueueUIThreadCallback(TRACK_REMOVED, receiver->track().release());
}

void Conductor::OnIceCandidate(const webrtc::IceCandidate* candidate) {
  RTC_LOG(LS_INFO) << __FUNCTION__ << " " << candidate->sdp_mline_index();
  // For loopback test. To save some connecting delay.
  if (loopback_) {
    if (!peer_connection_->AddIceCandidate(candidate)) {
      RTC_LOG(LS_WARNING) << "Failed to apply the received candidate";
    }
    return;
  }

  Json::Value jmessage;
  jmessage[kCandidateSdpMidName] = candidate->sdp_mid();
  jmessage[kCandidateSdpMlineIndexName] = candidate->sdp_mline_index();
  jmessage[kCandidateSdpName] = candidate->ToString();

  Json::StreamWriterBuilder factory;
  SendMessage(Json::writeString(factory, jmessage));
}

//
// PeerConnectionClientObserver implementation.
//

void Conductor::OnSignedIn() {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::OnDisconnected() {
  RTC_LOG(LS_INFO) << __FUNCTION__;

  DeletePeerConnection();

  if (main_wnd_->IsWindow())
    main_wnd_->SwitchToConnectUI();
}

void Conductor::OnPeerConnected(int id, const std::string& name) {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  // Refresh the list if we're showing it.
  if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
    main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::OnPeerDisconnected(int id) {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  if (id == peer_id_) {
    RTC_LOG(LS_INFO) << "Our peer disconnected";
    main_wnd_->QueueUIThreadCallback(PEER_CONNECTION_CLOSED, nullptr);
  } else {
    // Refresh the list if we're showing it.
    if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
      main_wnd_->SwitchToPeerList(client_->peers());
  }
}

void Conductor::OnMessageFromPeer(int peer_id, const std::string& message) {
  RTC_DCHECK(peer_id_ == peer_id || peer_id_ == -1);
  RTC_DCHECK(!message.empty());

  if (!peer_connection_.get()) {
    RTC_DCHECK(peer_id_ == -1);
    peer_id_ = peer_id;

    if (!InitializePeerConnection()) {
      RTC_LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
      client_->SignOut();
      return;
    }
  } else if (peer_id != peer_id_) {
    RTC_DCHECK(peer_id_ != -1);
    RTC_LOG(LS_WARNING)
        << "Received a message from unknown peer while already in a "
           "conversation with a different peer.";
    return;
  }

  Json::CharReaderBuilder factory;
  std::unique_ptr<Json::CharReader> reader =
      absl::WrapUnique(factory.newCharReader());
  Json::Value jmessage;
  if (!reader->parse(message.data(), message.data() + message.length(),
                     &jmessage, nullptr)) {
    RTC_LOG(LS_WARNING) << "Received unknown message. " << message;
    return;
  }
  std::string type_str;
  std::string json_object;

  webrtc::GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName,
                                  &type_str);
  if (!type_str.empty()) {
    if (type_str == "offer-loopback") {
      // This is a loopback call.
      // Recreate the peerconnection with DTLS disabled.
      if (!ReinitializePeerConnectionForLoopback()) {
        RTC_LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
        DeletePeerConnection();
        client_->SignOut();
      }
      return;
    }
    std::optional<webrtc::SdpType> type_maybe =
        webrtc::SdpTypeFromString(type_str);
    if (!type_maybe) {
      RTC_LOG(LS_ERROR) << "Unknown SDP type: " << type_str;
      return;
    }
    webrtc::SdpType type = *type_maybe;
    std::string sdp;
    if (!webrtc::GetStringFromJsonObject(jmessage, kSessionDescriptionSdpName,
                                         &sdp)) {
      RTC_LOG(LS_WARNING)
          << "Can't parse received session description message.";
      return;
    }
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(type, sdp, &error);
    if (!session_description) {
      RTC_LOG(LS_WARNING)
          << "Can't parse received session description message. "
             "SdpParseError was: "
          << error.description;
      return;
    }
    RTC_LOG(LS_INFO) << " Received session description :" << message;
    peer_connection_->SetRemoteDescription(
        DummySetSessionDescriptionObserver::Create().get(),
        session_description.release());
    if (type == webrtc::SdpType::kOffer) {
      peer_connection_->CreateAnswer(
          this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
    }
  } else {
    std::string sdp_mid;
    int sdp_mlineindex = 0;
    std::string sdp;
    if (!webrtc::GetStringFromJsonObject(jmessage, kCandidateSdpMidName,
                                         &sdp_mid) ||
        !webrtc::GetIntFromJsonObject(jmessage, kCandidateSdpMlineIndexName,
                                      &sdp_mlineindex) ||
        !webrtc::GetStringFromJsonObject(jmessage, kCandidateSdpName, &sdp)) {
      RTC_LOG(LS_WARNING) << "Can't parse received message.";
      return;
    }
    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidate> candidate(
        webrtc::CreateIceCandidate(sdp_mid, sdp_mlineindex, sdp, &error));
    if (!candidate.get()) {
      RTC_LOG(LS_WARNING) << "Can't parse received candidate message. "
                             "SdpParseError was: "
                          << error.description;
      return;
    }
    if (!peer_connection_->AddIceCandidate(candidate.get())) {
      RTC_LOG(LS_WARNING) << "Failed to apply the received candidate";
      return;
    }
    RTC_LOG(LS_INFO) << " Received candidate :" << message;
  }
}

void Conductor::OnMessageSent(int err) {
  // Process the next pending message if any.
  main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, nullptr);
}

void Conductor::OnServerConnectionFailure() {
  main_wnd_->MessageBox("Error", ("Failed to connect to " + server_).c_str(),
                        true);
}

//
// MainWndCallback implementation.
//

void Conductor::StartLogin(const std::string& server, int port) {
  if (client_->is_connected())
    return;
  server_ = server;
  client_->Connect(server, port, GetPeerName());
}

void Conductor::DisconnectFromServer() {
  if (client_->is_connected())
    client_->SignOut();
}

void Conductor::ConnectToPeer(int peer_id) {
  RTC_DCHECK(peer_id_ == -1);
  RTC_DCHECK(peer_id != -1);

  if (peer_connection_.get()) {
    main_wnd_->MessageBox(
        "Error", "We only support connecting to one peer at a time", true);
    return;
  }

  if (InitializePeerConnection()) {
    peer_id_ = peer_id;
    peer_connection_->CreateOffer(
        this, webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  } else {
    main_wnd_->MessageBox("Error", "Failed to initialize PeerConnection", true);
  }
}

void Conductor::AddTracks() {
  if (!peer_connection_->GetSenders().empty()) {
    return;  // Already added tracks.
  }

  webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(
      peer_connection_factory_->CreateAudioTrack(
          kAudioLabel,
          peer_connection_factory_->CreateAudioSource(webrtc::AudioOptions())
              .get()));
  auto result_or_error = peer_connection_->AddTrack(audio_track, {kStreamId});
  if (!result_or_error.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to add audio track to PeerConnection: "
                      << result_or_error.error().message();
  }

  webrtc::scoped_refptr<CapturerTrackSource> video_device =
      CapturerTrackSource::Create(env_.task_queue_factory());
  if (video_device) {
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_(
        peer_connection_factory_->CreateVideoTrack(video_device, kVideoLabel));
    main_wnd_->StartLocalRenderer(video_track_.get());

    result_or_error = peer_connection_->AddTrack(video_track_, {kStreamId});
    if (!result_or_error.ok()) {
      RTC_LOG(LS_ERROR) << "Failed to add video track to PeerConnection: "
                        << result_or_error.error().message();
    } else {
      ApplyLabSenderNoResizeDefaults(peer_connection_.get());
    }
  } else {
    RTC_LOG(LS_ERROR) << "OpenVideoCaptureDevice failed";
  }

  main_wnd_->SwitchToStreamingUI();
}

void Conductor::DisconnectFromCurrentPeer() {
  RTC_LOG(LS_INFO) << __FUNCTION__;
  if (peer_connection_.get()) {
    client_->SendHangUp(peer_id_);
    DeletePeerConnection();
  }

  if (main_wnd_->IsWindow())
    main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::UIThreadCallback(int msg_id, void* data) {
  switch (msg_id) {
    case PEER_CONNECTION_CLOSED:
      RTC_LOG(LS_INFO) << "PEER_CONNECTION_CLOSED";
      DeletePeerConnection();

      if (main_wnd_->IsWindow()) {
        if (client_->is_connected()) {
          main_wnd_->SwitchToPeerList(client_->peers());
        } else {
          main_wnd_->SwitchToConnectUI();
        }
      } else {
        DisconnectFromServer();
      }
      break;

    case SEND_MESSAGE_TO_PEER: {
      RTC_LOG(LS_INFO) << "SEND_MESSAGE_TO_PEER";
      std::string* msg = reinterpret_cast<std::string*>(data);
      if (msg) {
        // For convenience, we always run the message through the queue.
        // This way we can be sure that messages are sent to the server
        // in the same order they were signaled without much hassle.
        pending_messages_.push_back(msg);
      }

      if (!pending_messages_.empty() && !client_->IsSendingMessage()) {
        msg = pending_messages_.front();
        pending_messages_.pop_front();

        if (!client_->SendToPeer(peer_id_, *msg) && peer_id_ != -1) {
          RTC_LOG(LS_ERROR) << "SendToPeer failed";
          DisconnectFromServer();
        }
        delete msg;
      }

      if (!peer_connection_.get())
        peer_id_ = -1;

      break;
    }

    case NEW_TRACK_ADDED: {
      auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
      if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
        auto* video_track = static_cast<webrtc::VideoTrackInterface*>(track);
        main_wnd_->StartRemoteRenderer(video_track);
      }
      track->Release();
      break;
    }

    case TRACK_REMOVED: {
      // Remote peer stopped sending a track.
      auto* track = reinterpret_cast<webrtc::MediaStreamTrackInterface*>(data);
      track->Release();
      break;
    }

    default:
      RTC_DCHECK_NOTREACHED();
      break;
  }
}

void Conductor::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
  peer_connection_->SetLocalDescription(
      DummySetSessionDescriptionObserver::Create().get(), desc);

  std::string sdp;
  desc->ToString(&sdp);

  // For loopback test. To save some connecting delay.
  if (loopback_) {
    // Replace message type from "offer" to "answer"
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp);
    peer_connection_->SetRemoteDescription(
        DummySetSessionDescriptionObserver::Create().get(),
        session_description.release());
    return;
  }

  Json::Value jmessage;
  jmessage[kSessionDescriptionTypeName] =
      webrtc::SdpTypeToString(desc->GetType());
  jmessage[kSessionDescriptionSdpName] = sdp;

  Json::StreamWriterBuilder factory;
  SendMessage(Json::writeString(factory, jmessage));
}

void Conductor::OnFailure(webrtc::RTCError error) {
  RTC_LOG(LS_ERROR) << ToString(error.type()) << ": " << error.message();
}

void Conductor::SendMessage(const std::string& json_object) {
  std::string* msg = new std::string(json_object);
  main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, msg);
}

void Conductor::StartMetricsLoggingIfNeeded() {
  if (!absl::GetFlag(FLAGS_record_test_metrics) || !peer_connection_) {
    return;
  }
  if (metrics_logging_active_.load()) {
    return;
  }

  const int duration_s = std::max(1, absl::GetFlag(FLAGS_metrics_duration_seconds));
  const int interval_s = std::max(1, absl::GetFlag(FLAGS_metrics_interval_seconds));
  const int sample_count = std::max(1, duration_s / interval_s);
  const std::string output_path = absl::GetFlag(FLAGS_metrics_output_path);

  auto peer_connection_snapshot = peer_connection_;
  metrics_logger_ = webrtc::make_ref_counted<PeriodicMetricsLogger>(output_path);
  metrics_logging_active_.store(true);
  metrics_logging_thread_ = std::thread([this, peer_connection_snapshot,
                                         sample_count, interval_s]() {
    for (int i = 0; i < sample_count && metrics_logging_active_.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(interval_s));
      if (!metrics_logging_active_.load()) {
        break;
      }
      if (peer_connection_snapshot && metrics_logger_) {
        peer_connection_snapshot->GetStats(metrics_logger_.get());
      }
    }
    metrics_logging_active_.store(false);
  });
  RTC_LOG(LS_INFO) << "Started metrics logging: duration=" << duration_s
                   << "s, interval=" << interval_s
                   << "s, output=" << output_path;
}

void Conductor::StopMetricsLogging() {
  const bool was_active = metrics_logging_active_.exchange(false);
  if (metrics_logging_thread_.joinable()) {
    metrics_logging_thread_.join();
  }
  metrics_logger_ = nullptr;
  if (was_active) {
    RTC_LOG(LS_INFO) << "Stopped metrics logging.";
  }
}
