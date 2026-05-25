#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "api/environment/environment.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "examples/janus_client/janus_client.h"
#include "examples/janus_client/janus_conductor.h"
#include "rtc_base/logging.h"
#include "rtc_base/crypto_random.h"
#include "rtc_base/ssl_adapter.h"
#include "api/make_ref_counted.h"

ABSL_FLAG(std::string,
          janus_host,
          "localhost",
          "Hostname of the Janus Gateway HTTP endpoint.");
ABSL_FLAG(int,
          janus_port,
          8088,
          "Port of the Janus Gateway HTTP endpoint (default REST port).");
ABSL_FLAG(std::string,
          username,
          "",
          "Username to register with Janus. Defaults to USERNAME@hostname.");
ABSL_FLAG(std::string,
          callee,
          "",
          "Optional peer username to call automatically on startup.");
ABSL_FLAG(std::string,
          force_fieldtrials,
          "",
          "Field trials string passed to WebRTC.");

namespace {
constexpr std::chrono::milliseconds kRegistrationTimeout{30000};

std::string GenerateDefaultUsername() {
  return "janus_user_" + std::to_string(webrtc::CreateRandomId());
}
}

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  const std::string host = absl::GetFlag(FLAGS_janus_host);
  const int port = absl::GetFlag(FLAGS_janus_port);
  std::string username = absl::GetFlag(FLAGS_username);
  const std::string callee = absl::GetFlag(FLAGS_callee);
  const std::string field_trials = absl::GetFlag(FLAGS_force_fieldtrials);

  if (username.empty()) {
    username = GenerateDefaultUsername();
  }

  if (host.empty()) {
    std::cerr << "Janus host must not be empty" << std::endl;
    return -1;
  }
  if (port <= 0 || port > 65535) {
    std::cerr << "Invalid Janus port: " << port << std::endl;
    return -1;
  }

  webrtc::InitializeSSL();

  // Silence WebRTC's verbose logging; rely on our own targeted output.
  webrtc::LogMessage::LogToDebug(webrtc::LS_NONE);
  
  webrtc::Environment env =
      webrtc::CreateEnvironment(std::make_unique<webrtc::FieldTrials>(field_trials));

  JanusClient janus_client(host, port, env);
  auto conductor = webrtc::make_ref_counted<JanusConductor>(env, &janus_client);

  if (!conductor->Initialize()) {
    std::cerr << "Failed to initialize Janus conductor" << std::endl;
    webrtc::CleanupSSL();
    return -1;
  }

  if (!janus_client.ConnectAndRegister(username)) {
    std::cerr << "Failed to connect/register Janus client" << std::endl;
    webrtc::CleanupSSL();
    return -1;
  }

  janus_client.StartPolling();

  if (!conductor->WaitForRegistration(kRegistrationTimeout)) {
    std::cerr << "Timed out waiting for Janus registration" << std::endl;
    janus_client.Stop();
    webrtc::CleanupSSL();
    return -1;
  }

  conductor->StartNetworkStatsLogging(std::chrono::seconds(2));

  std::cout << "Registered as " << username
            << ", ready to communicate via Janus." << std::endl;
  std::cout << "Commands: call <peer>, hangup, msg <text>, quit" << std::endl;

  std::atomic<bool> running{true};

  std::thread input_thread([&running, conductor] {
    std::string line;
    while (running.load() && std::getline(std::cin, line)) {
      if (line == "quit" || line == "exit") {
        running = false;
        break;
      }
      if (line.rfind("call ", 0) == 0) {
        std::string target = line.substr(5);
        if (!target.empty()) {
          conductor->InitiateCall(target);
        }
        continue;
      }
      if (line.rfind("msg ", 0) == 0) {
        std::string message = line.substr(4);
        if (!message.empty()) {
          if (!conductor->SendChatMessage(message)) {
            std::cout << "Failed to send chat message" << std::endl;
          }
        }
        continue;
      }
      if (line == "hangup") {
        conductor->Disconnect();
        continue;
      }
      std::cout << "Commands: call <peer>, hangup, msg <text>, quit"
                << std::endl;
    }
    running = false;
  });

  if (!callee.empty()) {
    conductor->InitiateCall(callee);
  }

  while (running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  conductor->StopNetworkStatsLogging();
  conductor->Disconnect();
  janus_client.Stop();

  if (input_thread.joinable()) {
    input_thread.join();
  }

  webrtc::CleanupSSL();
  return 0;
}
