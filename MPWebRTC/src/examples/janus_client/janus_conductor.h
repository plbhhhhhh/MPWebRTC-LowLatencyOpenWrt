

#ifndef EXAMPLES_JANUS_CLIENT_JANUS_CONDUCTOR_H_
#define EXAMPLES_JANUS_CLIENT_JANUS_CONDUCTOR_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "api/data_channel_interface.h"
#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtp_receiver_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "examples/janus_client/janus_client.h"

namespace webrtc {
class Environment;
class Thread;
class RTCStatsCollectorCallback;
namespace test {
class VideoRenderer;
}  // namespace test
}  // namespace webrtc

namespace Json {
class Value;
}  // namespace Json

class JanusConductor : public webrtc::PeerConnectionObserver,
                       public webrtc::CreateSessionDescriptionObserver,
                       public webrtc::DataChannelObserver,
                       public JanusClientObserver {
 public:
  JanusConductor(const webrtc::Environment& env,
                 JanusClient* client);
  ~JanusConductor() override;

  bool Initialize();
  bool InitiateCall(const std::string& callee);
  void Disconnect();
  bool SendChatMessage(const std::string& message);
  void StartNetworkStatsLogging(std::chrono::milliseconds interval);
  void StopNetworkStatsLogging();

  // Blocks until a registration callback is observed or the timeout expires.
  bool WaitForRegistration(std::chrono::milliseconds timeout);

  // PeerConnectionObserver implementation.
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState
                             new_state) override {}
  void OnAddTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
                  const std::vector<webrtc::scoped_refptr<
                      webrtc::MediaStreamInterface>>& streams) override;
  void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface>
                         receiver) override;
  void OnDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
  void OnRenegotiationNeeded() override {}
  void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState
                                 new_state) override {}
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState
                                new_state) override {}
  void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
  void OnIceConnectionReceivingChange(bool receiving) override {}

  // CreateSessionDescriptionObserver implementation.
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
  void OnFailure(webrtc::RTCError error) override;

  // DataChannelObserver implementation.
  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer& buffer) override;

  // JanusClientObserver implementation.
  void OnJanusRegistered(const std::string& username) override;
  void OnJanusIncomingCall(const std::string& from,
                           const std::string& sdp) override;
  void OnJanusCallAccepted(const std::string& sdp) override;
  void OnJanusRemoteTrickle(const Json::Value& candidate) override;
  void OnJanusHangup(const std::string& reason) override;
  void OnJanusError(const std::string& error) override;

 private:
  enum class LocalAction {
    kNone,
    kCreateOffer,
    kCreateAnswer,
  };

  bool EnsurePeerConnection();
  void AddDefaultMedia();
  void ResetPeerConnection();
  void InitializeDataChannel();
  void AttachRemoteVideoSink(
      webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track);
  void LogNetworkStatsOnce();

  void SetRemoteDescription(webrtc::SdpType type, const std::string& sdp);

  class NetworkMetricsLogger;

  const webrtc::Environment env_;
  JanusClient* const janus_client_;

  std::unique_ptr<webrtc::Thread> signaling_thread_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
      peer_connection_factory_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;

  std::mutex mutex_;
  std::condition_variable registration_cv_;
  bool registered_ = false;
  bool has_active_call_ = false;
  LocalAction pending_action_ = LocalAction::kNone;
  std::string pending_remote_peer_;

  webrtc::scoped_refptr<webrtc::DataChannelInterface> chat_channel_;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> local_video_track_;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> remote_video_track_;

  class ConsoleVideoSink
      : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
   public:
    void OnFrame(const webrtc::VideoFrame& frame) override;

   private:
    int64_t last_logged_ms_ = 0;
  };
  std::unique_ptr<ConsoleVideoSink> remote_video_sink_;
  std::unique_ptr<webrtc::test::VideoRenderer> local_video_renderer_;
  std::unique_ptr<webrtc::test::VideoRenderer> remote_video_renderer_;

  std::atomic<bool> stats_logging_active_{false};
  std::chrono::milliseconds stats_logging_interval_{std::chrono::seconds(2)};
  std::thread stats_logging_thread_;
  webrtc::scoped_refptr<webrtc::RTCStatsCollectorCallback>
      network_metrics_logger_;
};

#endif  // EXAMPLES_JANUS_CLIENT_JANUS_CONDUCTOR_H_
