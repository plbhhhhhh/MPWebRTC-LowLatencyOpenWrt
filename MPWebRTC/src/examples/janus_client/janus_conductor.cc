
#include "examples/janus_client/janus_conductor.h"

#include <iostream>
#include <optional>
#include <sstream>
#include <utility>

#include "json/value.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtcstats_objects.h"
#include "api/stats/rtc_stats_report.h"
#include "rtc_base/ref_counted_object.h"

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/data_channel_interface.h"
#include "api/enable_media.h"
#include "api/make_ref_counted.h"
#include "api/test/create_frame_generator.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "test/frame_generator_capturer.h"
#include "test/platform_video_capturer.h"
#include "test/test_video_capturer.h"
#include "test/video_renderer.h"
#include "system_wrappers/include/clock.h"

namespace {
constexpr char kDefaultStunServer[] = "stun:stun.l.google.com:19302";
constexpr int kPreviewWidth = 640;
constexpr int kPreviewHeight = 480;

using webrtc::test::FrameGeneratorCapturer;
using webrtc::test::TestVideoCapturer;

std::unique_ptr<TestVideoCapturer> CreateCapturer(
    webrtc::TaskQueueFactory& task_queue_factory) {
  const size_t kWidth = 640;
  const size_t kHeight = 480;
  const size_t kFps = 30;
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (info) {
    const int num_devices = info->NumberOfDevices();
    for (int i = 0; i < num_devices; ++i) {
      char device_name[256];
      char unique_name[256];
      if (info->GetDeviceName(i, device_name, sizeof(device_name),
                              unique_name, sizeof(unique_name)) == -1) {
        RTC_LOG(LS_WARNING) << "Failed to query video device name for index "
                            << i;
        continue;
      }
      std::unique_ptr<TestVideoCapturer> capturer =
          webrtc::test::CreateVideoCapturer(kWidth, kHeight, kFps, i);
      if (capturer) {
        RTC_LOG(LS_INFO) << "Using physical camera: " << device_name
                         << " (" << unique_name << ")";
        return capturer;
      }
      RTC_LOG(LS_WARNING) << "Failed to initialize camera at index " << i
                          << ": " << device_name;
    }
  }

  auto frame_generator = webrtc::test::CreateSquareFrameGenerator(
      kWidth, kHeight, std::nullopt, std::nullopt);
  RTC_LOG(LS_WARNING)
    << "Falling back to synthetic video frames (no usable camera found).";
  return std::make_unique<FrameGeneratorCapturer>(
      webrtc::Clock::GetRealTimeClock(), std::move(frame_generator), kFps,
      task_queue_factory);
}

class CapturerTrackSource : public webrtc::VideoTrackSource {
 public:
  static webrtc::scoped_refptr<CapturerTrackSource> Create(
      webrtc::TaskQueueFactory& task_queue_factory) {
    std::unique_ptr<TestVideoCapturer> capturer =
        CreateCapturer(task_queue_factory);
    if (!capturer) {
      return nullptr;
    }
    capturer->Start();
    return webrtc::make_ref_counted<CapturerTrackSource>(std::move(capturer));
  }

 protected:
  explicit CapturerTrackSource(std::unique_ptr<TestVideoCapturer> capturer)
      : VideoTrackSource(/*remote=*/false),
        capturer_(std::move(capturer)) {}

 private:
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return capturer_.get();
  }

  std::unique_ptr<TestVideoCapturer> capturer_;
};

class DummySetSessionDescriptionObserver
    : public webrtc::SetSessionDescriptionObserver {
 public:
  static webrtc::scoped_refptr<DummySetSessionDescriptionObserver> Create() {
    return webrtc::make_ref_counted<DummySetSessionDescriptionObserver>();
  }
  void OnSuccess() override {}
  void OnFailure(webrtc::RTCError error) override {
    RTC_LOG(LS_ERROR) << "SetSessionDescription failed: " << error.message();
  }
};

}  // namespace

class JanusConductor::NetworkMetricsLogger
    : public webrtc::RTCStatsCollectorCallback {
 public:
  NetworkMetricsLogger() = default;

  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    double rtt_ms = -1.0;
    double available_out_bps = -1.0;
    double available_in_bps = -1.0;
    uint64_t inbound_packets = 0;
    uint64_t inbound_lost = 0;
    double outbound_loss_percent = -1.0;

    for (const auto& stats :
         report->GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
      const bool nominated = stats->nominated.value_or(false);
      const bool succeeded =
          stats->state.has_value() && stats->state.value() == "succeeded";
      if (!nominated && !succeeded) {
        continue;
      }
      if (stats->current_round_trip_time.has_value()) {
        rtt_ms = stats->current_round_trip_time.value() * 1000.0;
      }
      if (stats->available_outgoing_bitrate.has_value()) {
        available_out_bps = stats->available_outgoing_bitrate.value();
      }
      if (stats->available_incoming_bitrate.has_value()) {
        available_in_bps = stats->available_incoming_bitrate.value();
      }
    }

    for (const auto& inbound :
         report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
      inbound_packets += inbound->packets_received.value_or(0);
      const int32_t lost = inbound->packets_lost.value_or(0);
      if (lost > 0) {
        inbound_lost += static_cast<uint64_t>(lost);
      }
    }

    double outbound_loss_sum = 0.0;
    int outbound_loss_samples = 0;
    for (const auto& remote_inbound :
         report->GetStatsOfType<webrtc::RTCRemoteInboundRtpStreamStats>()) {
      if (remote_inbound->fraction_lost.has_value()) {
        outbound_loss_sum += remote_inbound->fraction_lost.value();
        ++outbound_loss_samples;
      }
    }
    if (outbound_loss_samples > 0) {
      outbound_loss_percent =
          (outbound_loss_sum / outbound_loss_samples) * 100.0;
    }

    const bool has_inbound_loss = (inbound_packets + inbound_lost) > 0;
    const double inbound_loss_percent = has_inbound_loss
                                            ? (static_cast<double>(inbound_lost) *
                                               100.0) /
                                                  static_cast<double>(inbound_packets +
                                                                      inbound_lost)
                                            : -1.0;

    if (rtt_ms < 0 && available_out_bps < 0 && available_in_bps < 0 &&
        inbound_loss_percent < 0 && outbound_loss_percent < 0) {
      return;  // Nothing meaningful to report yet.
    }

    std::ostringstream out;
    out << "[net] ";
    if (rtt_ms >= 0) {
      out << "rtt=" << rtt_ms << "ms ";
    }
    if (available_out_bps >= 0) {
      out << "uplink_bw=" << static_cast<int64_t>(available_out_bps / 1000.0)
          << "kbps ";
    }
    if (available_in_bps >= 0) {
      out << "downlink_bw=" << static_cast<int64_t>(available_in_bps / 1000.0)
          << "kbps ";
    }
    out << "in_loss=";
    if (inbound_loss_percent >= 0) {
      out << inbound_loss_percent;
    } else {
      out << "n/a";
    }
    out << "% out_loss=";
    if (outbound_loss_percent >= 0) {
      out << outbound_loss_percent;
    } else {
      out << "n/a";
    }
    out << "%";

    std::cout << out.str() << std::endl;
  }
};

JanusConductor::JanusConductor(const webrtc::Environment& env,
                               JanusClient* client)
    : env_(env), janus_client_(client) {
  RTC_CHECK(janus_client_);
  janus_client_->RegisterObserver(this);
}

JanusConductor::~JanusConductor() {
  StopNetworkStatsLogging();
  Disconnect();
}

void JanusConductor::StartNetworkStatsLogging(
    std::chrono::milliseconds interval) {
  stats_logging_interval_ = interval;
  if (stats_logging_active_.exchange(true)) {
    return;
  }
  network_metrics_logger_ = webrtc::make_ref_counted<NetworkMetricsLogger>();
  stats_logging_thread_ = std::thread([this]() {
    while (stats_logging_active_.load()) {
      LogNetworkStatsOnce();
      std::this_thread::sleep_for(stats_logging_interval_);
    }
  });
}

void JanusConductor::StopNetworkStatsLogging() {
  if (!stats_logging_active_.exchange(false)) {
    return;
  }
  if (stats_logging_thread_.joinable()) {
    stats_logging_thread_.join();
  }
  network_metrics_logger_ = nullptr;
}

void JanusConductor::LogNetworkStatsOnce() {
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pc_snapshot = peer_connection_;
  }
  if (!pc_snapshot || !network_metrics_logger_) {
    return;
  }
  pc_snapshot->GetStats(network_metrics_logger_.get());
}

bool JanusConductor::Initialize() {
  signaling_thread_ = webrtc::Thread::CreateWithSocketServer();
  signaling_thread_->Start();

  webrtc::PeerConnectionFactoryDependencies deps;
  deps.signaling_thread = signaling_thread_.get();
  deps.env = env_;
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  deps.video_encoder_factory = webrtc::CreateBuiltinVideoEncoderFactory();
  deps.video_decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();
  webrtc::EnableMedia(deps);

  peer_connection_factory_ =
      webrtc::CreateModularPeerConnectionFactory(std::move(deps));
  if (!peer_connection_factory_) {
    RTC_LOG(LS_ERROR) << "Failed to create PeerConnectionFactory";
    return false;
  }
  return true;
}

bool JanusConductor::InitiateCall(const std::string& callee) {
  if (!EnsurePeerConnection()) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_action_ = LocalAction::kCreateOffer;
    pending_remote_peer_ = callee;
  }
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  peer_connection_->CreateOffer(this, options);
  return true;
}

void JanusConductor::Disconnect() {
  if (janus_client_) {
    janus_client_->Hangup();
  }
  ResetPeerConnection();
}

bool JanusConductor::SendChatMessage(const std::string& message) {
  if (!chat_channel_) {
    RTC_LOG(LS_WARNING)
        << "Chat channel not ready; unable to send message.";
    return false;
  }
  if (chat_channel_->state() != webrtc::DataChannelInterface::kOpen) {
    RTC_LOG(LS_WARNING)
        << "Chat channel not open; current state="
        << webrtc::DataChannelInterface::DataStateString(
               chat_channel_->state());
    return false;
  }
  webrtc::DataBuffer buffer(message);
  if (!chat_channel_->Send(buffer)) {
    RTC_LOG(LS_WARNING) << "Failed to send chat message.";
    return false;
  }
  RTC_LOG(LS_INFO) << "Sent chat message: " << message;
  return true;
}

bool JanusConductor::WaitForRegistration(std::chrono::milliseconds timeout) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (registered_) {
      return true;
    }
  }
  if (janus_client_ && janus_client_->is_registered()) {
    std::lock_guard<std::mutex> lock(mutex_);
    registered_ = true;
    return true;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  return registration_cv_.wait_for(lock, timeout,
                                   [this] { return registered_; });
}

void JanusConductor::OnAddTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
    const std::vector<webrtc::scoped_refptr<webrtc::MediaStreamInterface>>&
        streams) {
  auto track = receiver->track();
  if (!track) {
    return;
  }
  if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
    RTC_LOG(LS_INFO) << "Remote video track added: " << receiver->id();
    AttachRemoteVideoSink(track);
  } else {
    RTC_LOG(LS_INFO) << "Remote track added: " << receiver->id();
  }
}

void JanusConductor::OnRemoveTrack(
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  auto track = receiver->track();
  if (track && track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind &&
      remote_video_track_ && track.get() == remote_video_track_.get()) {
    RTC_LOG(LS_INFO) << "Remote video track removed: " << receiver->id();
    if (remote_video_sink_ && remote_video_track_) {
      remote_video_track_->RemoveSink(remote_video_sink_.get());
    }
    if (remote_video_renderer_ && remote_video_track_) {
      remote_video_track_->RemoveSink(remote_video_renderer_.get());
    }
    remote_video_track_ = nullptr;
    remote_video_sink_.reset();
  } else {
    RTC_LOG(LS_INFO) << "Remote track removed: " << receiver->id();
  }
}

void JanusConductor::OnIceCandidate(const webrtc::IceCandidate* candidate) {
  std::string candidate_sdp;
  if (!candidate->ToString(&candidate_sdp)) {
    RTC_LOG(LS_WARNING) << "Failed to serialize local ICE candidate";
    return;
  }
  Json::Value json_candidate;
  json_candidate["candidate"] = candidate_sdp;
  json_candidate["sdpMid"] = candidate->sdp_mid();
  json_candidate["sdpMLineIndex"] = candidate->sdp_mline_index();
  janus_client_->SendTrickleCandidate(json_candidate);
}

void JanusConductor::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
  auto setter = DummySetSessionDescriptionObserver::Create();
  peer_connection_->SetLocalDescription(setter.get(), desc);

  std::string sdp;
  desc->ToString(&sdp);

  switch (desc->GetType()) {
    case webrtc::SdpType::kOffer:
      janus_client_->Call(pending_remote_peer_, sdp);
      break;
    case webrtc::SdpType::kAnswer:
      janus_client_->SendAnswer(sdp);
      break;
    default:
      break;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_action_ = LocalAction::kNone;
    has_active_call_ = true;
  }
}

void JanusConductor::OnFailure(webrtc::RTCError error) {
  RTC_LOG(LS_ERROR) << "Failed to create session description: "
                    << error.message();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_action_ = LocalAction::kNone;
  }
}

void JanusConductor::OnDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  if (!channel) {
    return;
  }
  RTC_LOG(LS_INFO) << "Data channel received from remote peer: "
                   << channel->label();
  if (chat_channel_) {
    chat_channel_->UnregisterObserver();
  }
  chat_channel_ = channel;
  chat_channel_->RegisterObserver(this);
}

void JanusConductor::OnStateChange() {
  if (!chat_channel_) {
    return;
  }
  RTC_LOG(LS_INFO) << "Chat channel state changed to "
                   << webrtc::DataChannelInterface::DataStateString(
                          chat_channel_->state());
}

void JanusConductor::OnMessage(const webrtc::DataBuffer& buffer) {
  std::string text(
      reinterpret_cast<const char*>(buffer.data.data()), buffer.data.size());
  if (!buffer.binary) {
    RTC_LOG(LS_INFO) << "Chat message received: " << text;
    std::cout << "[chat] " << text << std::endl;
  } else {
    RTC_LOG(LS_INFO) << "Binary message of size " << buffer.size()
                     << " received (ignored).";
  }
}

void JanusConductor::OnJanusRegistered(const std::string& username) {
  RTC_LOG(LS_INFO) << "Registered with Janus as " << username;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    registered_ = true;
  }
  registration_cv_.notify_all();
}

void JanusConductor::OnJanusIncomingCall(const std::string& from,
                                         const std::string& sdp) {
  RTC_LOG(LS_INFO) << "Incoming call from " << from;
  ResetPeerConnection();
  if (!EnsurePeerConnection()) {
    RTC_LOG(LS_ERROR) << "Failed to create PeerConnection for incoming call";
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_action_ = LocalAction::kCreateAnswer;
    pending_remote_peer_ = from;
  }
  SetRemoteDescription(webrtc::SdpType::kOffer, sdp);
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  peer_connection_->CreateAnswer(this, options);
}

void JanusConductor::OnJanusCallAccepted(const std::string& sdp) {
  RTC_LOG(LS_INFO) << "Call accepted";
  SetRemoteDescription(webrtc::SdpType::kAnswer, sdp);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    has_active_call_ = true;
  }
}

void JanusConductor::OnJanusRemoteTrickle(const Json::Value& candidate) {
  RTC_LOG(LS_INFO) << "Remote candidate received";
  if (!peer_connection_) {
    return;
  }
  const std::string sdp_mid = candidate["sdpMid"].asString();
  const int sdp_mline_index = candidate["sdpMLineIndex"].asInt();
  const std::string sdp = candidate["candidate"].asString();
  webrtc::SdpParseError error;
  std::unique_ptr<webrtc::IceCandidateInterface> ice_candidate(
      webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, sdp, &error));
  if (!ice_candidate) {
    RTC_LOG(LS_WARNING) << "Failed to parse ICE candidate: "
                        << error.description;
    return;
  }
  if (!peer_connection_->AddIceCandidate(ice_candidate.get())) {
    RTC_LOG(LS_WARNING) << "Failed to add ICE candidate";
  }
}

void JanusConductor::OnJanusHangup(const std::string& reason) {
  RTC_LOG(LS_INFO) << "Janus hangup: " << reason;
  ResetPeerConnection();
}

void JanusConductor::OnJanusError(const std::string& error) {
  RTC_LOG(LS_ERROR) << "Janus error: " << error;
}

bool JanusConductor::EnsurePeerConnection() {
  if (peer_connection_) {
    return true;
  }
  if (!peer_connection_factory_) {
    RTC_LOG(LS_ERROR) << "PeerConnectionFactory not ready";
    return false;
  }
  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  webrtc::PeerConnectionInterface::IceServer server;
  server.uri = kDefaultStunServer;
  config.servers.push_back(server);

  webrtc::PeerConnectionDependencies dependencies(this);
  auto result =
      peer_connection_factory_->CreatePeerConnectionOrError(
          config, std::move(dependencies));
  if (!result.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to create PeerConnection: "
                      << result.error().message();
    return false;
  }
  peer_connection_ = std::move(result.value());
  InitializeDataChannel();
  AddDefaultMedia();
  return true;
}

void JanusConductor::AddDefaultMedia() {
  if (!peer_connection_) {
    return;
  }
  bool has_audio = false;
  bool has_video = false;
  for (const auto& sender : peer_connection_->GetSenders()) {
    if (!sender) {
      continue;
    }
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track =
        sender->track();
    if (!track) {
      continue;
    }
    if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
      has_audio = true;
    } else if (track->kind() ==
               webrtc::MediaStreamTrackInterface::kVideoKind) {
      has_video = true;
    }
  }

  if (!has_audio) {
    webrtc::AudioOptions audio_options;
    auto audio_source =
        peer_connection_factory_->CreateAudioSource(audio_options);
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track =
        peer_connection_factory_->CreateAudioTrack("janus_audio",
                                                   audio_source.get());
    auto result_or_error =
        peer_connection_->AddTrack(audio_track, {"janus_stream"});
    if (!result_or_error.ok()) {
      RTC_LOG(LS_WARNING) << "Failed to add audio track: "
                          << result_or_error.error().message();
    }
  }

  if (!has_video) {
    auto video_source =
        CapturerTrackSource::Create(env_.task_queue_factory());
    if (video_source) {
      webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track =
          peer_connection_factory_->CreateVideoTrack(video_source,
                                                     "janus_video");
      auto video_result =
          peer_connection_->AddTrack(video_track, {"janus_stream"});
      if (!video_result.ok()) {
        RTC_LOG(LS_WARNING) << "Failed to add video track: "
                            << video_result.error().message();
      } else {
        local_video_track_ = std::move(video_track);
      }
    } else {
      RTC_LOG(LS_ERROR)
          << "Failed to create local video source; video disabled.";
    }
  }

  if (local_video_track_) {
    if (!local_video_renderer_) {
      local_video_renderer_.reset(webrtc::test::VideoRenderer::Create(
          "Janus Local Preview", kPreviewWidth, kPreviewHeight));
      if (!local_video_renderer_) {
        RTC_LOG(LS_WARNING)
            << "Failed to create local video renderer; preview disabled.";
      }
    }
    if (local_video_renderer_) {
      webrtc::VideoSinkWants wants;
      local_video_track_->AddOrUpdateSink(local_video_renderer_.get(), wants);
    }
  }
}

void JanusConductor::ResetPeerConnection() {
  if (chat_channel_) {
    chat_channel_->UnregisterObserver();
    chat_channel_->Close();
    chat_channel_ = nullptr;
  }
  if (local_video_track_ && local_video_renderer_) {
    local_video_track_->RemoveSink(local_video_renderer_.get());
  }
  if (remote_video_track_ && remote_video_sink_) {
    remote_video_track_->RemoveSink(remote_video_sink_.get());
  }
  if (remote_video_track_ && remote_video_renderer_) {
    remote_video_track_->RemoveSink(remote_video_renderer_.get());
  }
  remote_video_track_ = nullptr;
  remote_video_sink_.reset();
  local_video_track_ = nullptr;
  if (peer_connection_) {
    peer_connection_->Close();
    peer_connection_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    has_active_call_ = false;
    pending_action_ = LocalAction::kNone;
    pending_remote_peer_.clear();
  }
}

void JanusConductor::InitializeDataChannel() {
  if (!peer_connection_) {
    return;
  }
  if (chat_channel_) {
    chat_channel_->UnregisterObserver();
    chat_channel_->Close();
    chat_channel_ = nullptr;
  }

  webrtc::DataChannelInit init;
  init.ordered = true;
  auto channel_or_error =
      peer_connection_->CreateDataChannelOrError("chat", &init);
  if (!channel_or_error.ok()) {
    RTC_LOG(LS_WARNING) << "Failed to create chat data channel: "
                        << channel_or_error.error().message();
    return;
  }
  chat_channel_ = std::move(channel_or_error.value());
  chat_channel_->RegisterObserver(this);
  RTC_LOG(LS_INFO) << "Chat data channel created.";
}

void JanusConductor::AttachRemoteVideoSink(
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track) {
  if (!track || track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind) {
    return;
  }
  if (remote_video_track_) {
    if (remote_video_sink_) {
      remote_video_track_->RemoveSink(remote_video_sink_.get());
    }
    if (remote_video_renderer_) {
      remote_video_track_->RemoveSink(remote_video_renderer_.get());
    }
  }
  webrtc::VideoTrackInterface* video_track =
      static_cast<webrtc::VideoTrackInterface*>(track.get());
  remote_video_track_ = video_track;
  if (!remote_video_sink_) {
    remote_video_sink_ = std::make_unique<ConsoleVideoSink>();
  }
  webrtc::VideoSinkWants wants;
  remote_video_track_->AddOrUpdateSink(remote_video_sink_.get(), wants);
  if (!remote_video_renderer_) {
    remote_video_renderer_.reset(webrtc::test::VideoRenderer::Create(
        "Janus Remote Video", kPreviewWidth, kPreviewHeight));
    if (!remote_video_renderer_) {
      RTC_LOG(LS_WARNING)
          << "Failed to create remote video renderer; remote preview disabled.";
    }
  }
  if (remote_video_renderer_) {
    remote_video_track_->AddOrUpdateSink(remote_video_renderer_.get(), wants);
  }
  RTC_LOG(LS_INFO) << "Remote video sink attached.";
}

void JanusConductor::SetRemoteDescription(webrtc::SdpType type,
                                          const std::string& sdp) {
  if (!peer_connection_) {
    return;
  }
  webrtc::SdpParseError error;
  std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
      webrtc::CreateSessionDescription(type, sdp, &error);
  if (!session_description) {
    RTC_LOG(LS_WARNING) << "Failed to parse remote description: "
                        << error.description;
    return;
  }
  auto setter = DummySetSessionDescriptionObserver::Create();
  peer_connection_->SetRemoteDescription(setter.get(),
                                         session_description.release());
}

void JanusConductor::ConsoleVideoSink::OnFrame(
    const webrtc::VideoFrame& frame) {
  const int64_t now_ms = webrtc::TimeMillis();
  if (last_logged_ms_ == 0 || now_ms - last_logged_ms_ >= 2000) {
    last_logged_ms_ = now_ms;
    RTC_LOG(LS_INFO) << "Remote video frame " << frame.width() << "x"
                     << frame.height() << " ts_us=" << frame.timestamp_us();
  }
}
