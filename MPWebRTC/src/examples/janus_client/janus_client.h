
#ifndef EXAMPLES_JANUS_CLIENT_JANUS_CLIENT_H_
#define EXAMPLES_JANUS_CLIENT_JANUS_CLIENT_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "absl/base/nullability.h"
#include "api/environment/environment.h"
#include "rtc_base/socket_address.h"

namespace Json {
class Value;
}  // namespace Json

class JanusClientObserver {
 public:
  virtual void OnJanusRegistered(const std::string& username) = 0;
  virtual void OnJanusIncomingCall(const std::string& from,
                                   const std::string& sdp) = 0;
  virtual void OnJanusCallAccepted(const std::string& sdp) = 0;
  virtual void OnJanusRemoteTrickle(const Json::Value& candidate) = 0;
  virtual void OnJanusHangup(const std::string& reason) = 0;
  virtual void OnJanusError(const std::string& error) = 0;

 protected:
  virtual ~JanusClientObserver() = default;
};

class JanusClient {
 public:
  JanusClient(const std::string& server_host,
              int server_port,
              const webrtc::Environment& env);
  ~JanusClient();

  void RegisterObserver(JanusClientObserver* observer);

  // Creates a Janus session, attaches to the videocall plugin and registers
  // the provided username. Returns false if any of those steps fail.
  bool ConnectAndRegister(const std::string& username);

  // Initiates a call to |callee| with the given local SDP offer.
  bool Call(const std::string& callee, const std::string& sdp_offer);

  // Sends an SDP answer for an incoming call.
  bool SendAnswer(const std::string& sdp_answer);

  // Sends a trickle ICE candidate.
  bool SendTrickleCandidate(const Json::Value& candidate);

  // Sends a hangup request to Janus.
  bool Hangup();

  // Starts/Stops the long-poll loop that receives Janus events. Polling is
  // automatically stopped in the destructor.
  void StartPolling();
  void Stop();

  const std::string& username() const { return username_; }
  bool is_registered() const { return registered_; }

 private:
  bool EnsureSession();
  bool EnsureHandle();

  bool HttpRequest(const std::string& method,
                   const std::string& path,
                   const std::string& payload,
           Json::Value* response,
           std::chrono::milliseconds timeout =
             std::chrono::seconds(10));

  bool CreateSession(Json::Value* response);
  bool AttachPlugin(Json::Value* response);
  bool RegisterUser(const std::string& username);

  void PollLoop();
  bool PollOnce();
  bool HandleEvent(const Json::Value& message);

  std::string BuildPath() const;
  std::string BuildPathWithHandle() const;
  std::string NewTransaction() const;
  bool ValidateSuccessResponse(const Json::Value& response,
                               const char* context,
                               Json::Value* data_out);
  bool ExtractId(const Json::Value& data,
                 const char* context,
                 uint64_t* id_out);
  void LogUnexpectedResponse(const char* context,
                             const Json::Value& response);

  const webrtc::Environment env_;
  const std::string server_host_;
  webrtc::SocketAddress server_address_;
  JanusClientObserver* observer_ = nullptr;

  std::string username_;
  uint64_t session_id_ = 0;
  uint64_t handle_id_ = 0;

  std::thread poll_thread_;
  std::atomic<bool> stop_polling_{true};
  std::atomic<bool> registered_{false};
};

#endif  // EXAMPLES_JANUS_CLIENT_JANUS_CLIENT_H_
