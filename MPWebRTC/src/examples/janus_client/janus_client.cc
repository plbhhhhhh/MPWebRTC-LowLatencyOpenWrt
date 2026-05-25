

#include "examples/janus_client/janus_client.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "absl/strings/str_format.h"
#include "api/units/time_delta.h"
#include "rtc_base/checks.h"
#include "rtc_base/crypto_random.h"
#include "rtc_base/logging.h"
#include "rtc_base/physical_socket_server.h"
#include "rtc_base/socket.h"
#include "third_party/jsoncpp/source/include/json/json.h"

namespace {

constexpr char kJanusPluginVideocall[] = "janus.plugin.videocall";

std::string_view TrimStringView(std::string_view sv) {
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
    sv.remove_suffix(1);
  }
  return sv;
}

struct HttpMetadata {
  std::optional<size_t> content_length;
  bool chunked = false;
};

HttpMetadata ParseHttpMetadata(std::string_view header) {
  HttpMetadata metadata;
  size_t pos = 0;
  while (pos < header.size()) {
    size_t line_end = header.find("\r\n", pos);
    const size_t length =
        (line_end == std::string::npos ? header.size() : line_end) - pos;
    std::string_view line = header.substr(pos, length);
    pos = (line_end == std::string::npos) ? header.size() : line_end + 2;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string key(line.substr(0, colon));
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::string_view value = TrimStringView(line.substr(colon + 1));
    if (key == "content-length") {
      size_t content_length = 0;
      auto [ptr, ec] = std::from_chars(value.data(),
                                       value.data() + value.size(),
                                       content_length);
      if (ec == std::errc() && ptr == value.data() + value.size()) {
        metadata.content_length = content_length;
      }
    } else if (key == "transfer-encoding") {
      std::string lower_value(value);
      std::transform(lower_value.begin(), lower_value.end(),
                     lower_value.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (lower_value.find("chunked") != std::string::npos) {
        metadata.chunked = true;
      }
    }
  }
  return metadata;
}

bool IsChunkedBodyComplete(std::string_view body) {
  size_t pos = 0;
  while (pos < body.size()) {
    size_t line_end = body.find("\r\n", pos);
    if (line_end == std::string::npos) {
      return false;
    }
    std::string_view size_line = body.substr(pos, line_end - pos);
    pos = line_end + 2;
    size_line = TrimStringView(size_line);
    if (size_line.empty()) {
      continue;
    }
    const size_t semicolon = size_line.find(';');
    if (semicolon != std::string::npos) {
      size_line = size_line.substr(0, semicolon);
      size_line = TrimStringView(size_line);
    }
    size_t chunk_size = 0;
    auto [ptr, ec] =
        std::from_chars(size_line.data(),
                        size_line.data() + size_line.size(),
                        chunk_size, 16);
    if (ec != std::errc() ||
        ptr != size_line.data() + size_line.size()) {
      return false;
    }
    if (chunk_size == 0) {
      if (body.size() >= pos + 2 && body.substr(pos, 2) == "\r\n") {
        return true;
      }
      size_t trailer_end = body.find("\r\n\r\n", pos);
      return trailer_end != std::string::npos;
    }
    if (body.size() < pos + chunk_size + 2) {
      return false;
    }
    pos += chunk_size;
    if (body.substr(pos, 2) != "\r\n") {
      return false;
    }
    pos += 2;
  }
  return false;
}

bool DecodeChunkedBody(std::string_view body, std::string* out) {
  RTC_DCHECK(out);
  out->clear();
  size_t pos = 0;
  while (pos < body.size()) {
    size_t line_end = body.find("\r\n", pos);
    if (line_end == std::string::npos) {
      return false;
    }
    std::string_view size_line = body.substr(pos, line_end - pos);
    pos = line_end + 2;
    size_line = TrimStringView(size_line);
    if (size_line.empty()) {
      continue;
    }
    const size_t semicolon = size_line.find(';');
    if (semicolon != std::string::npos) {
      size_line = size_line.substr(0, semicolon);
      size_line = TrimStringView(size_line);
    }
    size_t chunk_size = 0;
    auto [ptr, ec] =
        std::from_chars(size_line.data(),
                        size_line.data() + size_line.size(),
                        chunk_size, 16);
    if (ec != std::errc() ||
        ptr != size_line.data() + size_line.size()) {
      return false;
    }
    if (chunk_size == 0) {
      if (body.size() >= pos + 2 && body.substr(pos, 2) == "\r\n") {
        return true;
      }
      size_t trailer_end = body.find("\r\n\r\n", pos);
      return trailer_end != std::string::npos;
    }
    if (body.size() < pos + chunk_size + 2) {
      return false;
    }
    out->append(body.substr(pos, chunk_size));
    pos += chunk_size;
    if (body.substr(pos, 2) != "\r\n") {
      return false;
    }
    pos += 2;
  }
  return false;
}

}  // namespace

JanusClient::JanusClient(const std::string& server_host,
                         int server_port,
                         const webrtc::Environment& env)
    : env_(env),
      server_host_(server_host),
      server_address_(server_host, server_port) {}

JanusClient::~JanusClient() {
  Stop();
}

void JanusClient::RegisterObserver(JanusClientObserver* observer) {
  observer_ = observer;
}

bool JanusClient::ConnectAndRegister(const std::string& username) {
  registered_ = false;
  if (!EnsureSession()) {
    return false;
  }
  if (!EnsureHandle()) {
    return false;
  }
  if (!RegisterUser(username)) {
    return false;
  }
  username_ = username;
  return true;
}

bool JanusClient::Call(const std::string& callee, const std::string& sdp_offer) {
  if (!EnsureSession() || !EnsureHandle()) {
    return false;
  }
  Json::Value payload;
  payload["janus"] = "message";
  payload["transaction"] = NewTransaction();
  Json::Value& body = payload["body"];
  body["request"] = "call";
  body["username"] = callee;
  Json::Value& jsep = payload["jsep"];
  jsep["type"] = "offer";
  jsep["sdp"] = sdp_offer;

  Json::Value response;
  if (!HttpRequest("POST", BuildPathWithHandle(),
                   Json::writeString(Json::StreamWriterBuilder(), payload),
                   &response)) {
    return false;
  }
  // Janus typically returns {"janus":"ack"} for plugin messages.
  return true;
}

bool JanusClient::SendAnswer(const std::string& sdp_answer) {
  if (!EnsureSession() || !EnsureHandle()) {
    return false;
  }
  Json::Value payload;
  payload["janus"] = "message";
  payload["transaction"] = NewTransaction();
  Json::Value& body = payload["body"];
  body["request"] = "accept";
  Json::Value& jsep = payload["jsep"];
  jsep["type"] = "answer";
  jsep["sdp"] = sdp_answer;

  Json::Value response;
  if (!HttpRequest("POST", BuildPathWithHandle(),
                   Json::writeString(Json::StreamWriterBuilder(), payload),
                   &response)) {
    return false;
  }
  return true;
}

bool JanusClient::SendTrickleCandidate(const Json::Value& candidate) {
  if (!EnsureSession() || !EnsureHandle()) {
    return false;
  }
  Json::Value payload;
  payload["janus"] = "trickle";
  payload["transaction"] = NewTransaction();
  payload["candidate"] = candidate;

  Json::Value response;
  if (!HttpRequest("POST", BuildPathWithHandle(),
                   Json::writeString(Json::StreamWriterBuilder(), payload),
                   &response)) {
    return false;
  }
  return true;
}

bool JanusClient::Hangup() {
  Json::Value payload;
  payload["janus"] = "message";
  payload["transaction"] = NewTransaction();
  payload["body"]["request"] = "hangup";

  Json::Value response;
  return HttpRequest("POST", BuildPathWithHandle(),
                     Json::writeString(Json::StreamWriterBuilder(), payload),
                     &response);
}

void JanusClient::StartPolling() {
  if (!session_id_) {
    RTC_LOG(LS_ERROR) << "Cannot start polling before session exists";
    return;
  }
  if (!stop_polling_) {
    return;  // Already polling.
  }
  stop_polling_ = false;
  poll_thread_ = std::thread([this] { PollLoop(); });
}

void JanusClient::Stop() {
  stop_polling_ = true;
  if (poll_thread_.joinable()) {
    poll_thread_.join();
  }
}

bool JanusClient::EnsureSession() {
  if (session_id_ != 0) {
    return true;
  }
  Json::Value response;
  if (!CreateSession(&response)) {
    return false;
  }
  Json::Value data;
  if (!ValidateSuccessResponse(response, "create session", &data)) {
    return false;
  }
  if (!ExtractId(data, "create session", &session_id_)) {
    return false;
  }
  return session_id_ != 0;
}

bool JanusClient::EnsureHandle() {
  if (handle_id_ != 0) {
    return true;
  }
  Json::Value response;
  if (!AttachPlugin(&response)) {
    return false;
  }
  Json::Value data;
  if (!ValidateSuccessResponse(response, "attach plugin", &data)) {
    return false;
  }
  if (!ExtractId(data, "attach plugin", &handle_id_)) {
    return false;
  }
  return handle_id_ != 0;
}

bool JanusClient::HttpRequest(const std::string& method,
                              const std::string& path,
                              const std::string& payload,
                              Json::Value* response,
                              std::chrono::milliseconds timeout) {
  RTC_DCHECK(response);
  webrtc::PhysicalSocketServer socket_server;
  const int family = server_address_.ipaddr().family() == AF_UNSPEC
                         ? AF_INET
                         : server_address_.ipaddr().family();
  std::unique_ptr<webrtc::Socket> socket(
      socket_server.CreateSocket(family, SOCK_STREAM));
  if (!socket) {
    RTC_LOG(LS_ERROR) << "Failed to create socket";
    return false;
  }
  if (socket->Connect(server_address_) == SOCKET_ERROR) {
    RTC_LOG(LS_ERROR) << "Failed to connect to Janus server at "
                      << server_address_.ToString();
    socket->Close();
    return false;
  }

  const std::string host_header =
      server_host_.empty() ? server_address_.ToString() : server_host_;
  std::string request;
  request.reserve(payload.size() + path.size() + host_header.size() + 128);
  request.append(method).append(" ").append(path).append(" HTTP/1.1\r\n");
  request.append("Host: ").append(host_header).append("\r\n");
  request.append("Connection: close\r\n");
  if (!payload.empty()) {
    request.append("Content-Type: application/json\r\n");
    request.append(absl::StrFormat("Content-Length: %zu\r\n", payload.size()));
  }
  request.append("\r\n");
  if (!payload.empty()) {
    request.append(payload);
  }

  size_t sent = 0;
  const char* data = request.c_str();
  size_t remaining = request.size();
  while (remaining > 0) {
    int bytes = socket->Send(data + sent, remaining);
    if (bytes <= 0) {
      RTC_LOG(LS_ERROR) << "Failed to send HTTP request";
      socket->Close();
      return false;
    }
    sent += bytes;
    remaining -= bytes;
  }

  std::string raw_response;
  char buffer[4096];
  const auto start_time = std::chrono::steady_clock::now();
  auto overall_deadline = start_time + timeout;
  auto completion_deadline = overall_deadline;
  size_t header_end_pos = std::string::npos;
  HttpMetadata metadata;
  while (true) {
    int bytes = socket->Recv(buffer, sizeof(buffer), nullptr);
    if (bytes > 0) {
      completion_deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(5);
      if (completion_deadline > overall_deadline) {
        completion_deadline = overall_deadline;
      }
      raw_response.append(buffer, bytes);
      if (header_end_pos == std::string::npos) {
        header_end_pos = raw_response.find("\r\n\r\n");
        if (header_end_pos != std::string::npos) {
          metadata = ParseHttpMetadata(
              std::string_view(raw_response.data(), header_end_pos));
        }
      }
      if (header_end_pos != std::string::npos) {
        const size_t body_start = header_end_pos + 4;
     const size_t body_size = raw_response.size() - body_start;
     if ((metadata.content_length &&
       body_size >= metadata.content_length.value()) ||
      (metadata.chunked &&
       IsChunkedBodyComplete(raw_response.substr(body_start)))) {
          break;
        }
        if (!metadata.content_length && !metadata.chunked) {
          const std::string body_candidate = raw_response.substr(body_start);
          if (!body_candidate.empty()) {
            Json::CharReaderBuilder temp_factory;
            std::unique_ptr<Json::CharReader> temp_reader(
                temp_factory.newCharReader());
            Json::Value temp_json;
            std::string temp_errs;
            if (temp_reader->parse(body_candidate.data(),
                                   body_candidate.data() +
                                       body_candidate.size(),
                                   &temp_json, &temp_errs)) {
              break;
            }
          }
        }
      }
      continue;
    }

    const int err = socket->GetError();
    if (bytes == 0) {
      break;
    }

    if (webrtc::IsBlockingError(err)) {
      const auto now = std::chrono::steady_clock::now();
      const auto active_deadline =
          header_end_pos == std::string::npos ? overall_deadline
                                               : completion_deadline;
      if (now > active_deadline) {
        RTC_LOG(LS_WARNING) << "Timed out waiting for HTTP response on "
                            << method << " " << path;
        socket->Close();
        return false;
      }
      if (header_end_pos != std::string::npos) {
        const size_t body_start = header_end_pos + 4;
        const size_t body_size = raw_response.size() - body_start;
        if ((metadata.content_length &&
             body_size >= metadata.content_length.value()) ||
            (metadata.chunked &&
             IsChunkedBodyComplete(raw_response.substr(body_start)))) {
          break;
        }
      }
      socket_server.Wait(webrtc::TimeDelta::Millis(50), /*process_io=*/true);
      continue;
    }

    RTC_LOG(LS_ERROR) << "Error receiving HTTP response, err=" << err;
    socket->Close();
    return false;
  }

  socket->Close();

  if (header_end_pos == std::string::npos) {
    header_end_pos = raw_response.find("\r\n\r\n");
  }
  if (header_end_pos == std::string::npos) {
    RTC_LOG(LS_ERROR) << "Malformed HTTP response: " << raw_response;
    return false;
  }
  const std::string header = raw_response.substr(0, header_end_pos);
  std::string body = raw_response.substr(header_end_pos + 4);

  HttpMetadata final_metadata = ParseHttpMetadata(header);
  if (final_metadata.chunked) {
    std::string decoded;
    if (!DecodeChunkedBody(body, &decoded)) {
      RTC_LOG(LS_ERROR) << "Failed to decode chunked HTTP body: " << body;
      return false;
    }
    body.swap(decoded);
  } else if (final_metadata.content_length &&
             body.size() > final_metadata.content_length.value()) {
    body.resize(final_metadata.content_length.value());
  }

  const size_t status_end = header.find("\r\n");
  const std::string_view status_line =
      status_end == std::string::npos
          ? std::string_view(header)
          : std::string_view(header).substr(0, status_end);
  if (status_line.find(" 200 ") == std::string::npos &&
      status_line.find(" 201 ") == std::string::npos &&
      status_line.find(" 202 ") == std::string::npos) {
    RTC_LOG(LS_ERROR) << "Unexpected HTTP status: " << status_line
                      << " body: " << body;
    return false;
  }

  Json::CharReaderBuilder factory;
  std::string errs;
  std::unique_ptr<Json::CharReader> reader(factory.newCharReader());
  if (!reader->parse(body.data(), body.data() + body.size(), response, &errs)) {
    RTC_LOG(LS_ERROR) << "Failed to parse JSON body: " << errs
                      << " body: " << body;
    return false;
  }
  return true;
}

bool JanusClient::CreateSession(Json::Value* response) {
  Json::Value body;
  body["janus"] = "create";
  body["transaction"] = NewTransaction();

  return HttpRequest("POST", "/janus",
                     Json::writeString(Json::StreamWriterBuilder(), body),
                     response);
}

bool JanusClient::AttachPlugin(Json::Value* response) {
  Json::Value body;
  body["janus"] = "attach";
  body["plugin"] = kJanusPluginVideocall;
  body["transaction"] = NewTransaction();

  return HttpRequest("POST", BuildPath(),
                     Json::writeString(Json::StreamWriterBuilder(), body),
                     response);
}

bool JanusClient::RegisterUser(const std::string& username) {
  Json::Value body;
  body["janus"] = "message";
  body["transaction"] = NewTransaction();
  body["body"]["request"] = "register";
  body["body"]["username"] = username;

  Json::Value response;
  if (!HttpRequest("POST", BuildPathWithHandle(),
                   Json::writeString(Json::StreamWriterBuilder(), body),
                   &response)) {
    return false;
  }
  RTC_LOG(LS_INFO) << "Register response: "
                   << Json::writeString(Json::StreamWriterBuilder(),
                                        response);
  return true;
}

void JanusClient::PollLoop() {
  while (!stop_polling_) {
    if (!PollOnce()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }
}

bool JanusClient::PollOnce() {
  if (!session_id_) {
    return false;
  }
  const uint64_t rid = webrtc::CreateRandomId64();
  std::string path = absl::StrFormat("/janus/%llu?rid=%llu&maxev=1",
                                     static_cast<unsigned long long>(session_id_),
                                     static_cast<unsigned long long>(rid));
  Json::Value response;
  if (!HttpRequest("GET", path, /*payload=*/"", &response,
                   std::chrono::seconds(60))) {
    return false;
  }
  RTC_LOG(LS_INFO) << "PollOnce response: "
                   << Json::writeString(Json::StreamWriterBuilder(),
                                        response);
  return HandleEvent(response);
}

bool JanusClient::HandleEvent(const Json::Value& message) {
  if (message.isNull()) {
    return true;
  }
  const std::string janus_type = message["janus"].asString();
  if (janus_type == "keepalive" || janus_type == "ack") {
    return true;
  }
  if (janus_type == "event") {
    const Json::Value plugindata = message["plugindata"];
    const Json::Value data = plugindata["data"];
    const Json::Value& payload =
        (data.isObject() && data["result"].isObject()) ? data["result"] : data;
    const std::string event = payload["event"].asString();
    if (event == "registered") {
      registered_ = true;
      if (observer_) {
        const std::string registered_username =
            !payload["username"].isNull() ? payload["username"].asString()
                                           : username_;
        observer_->OnJanusRegistered(registered_username);
      }
      return true;
    }
    if (event == "incomingcall") {
      const std::string from = payload["username"].asString();
      const Json::Value jsep = message["jsep"];
      if (observer_ && jsep.isObject()) {
        observer_->OnJanusIncomingCall(from, jsep["sdp"].asString());
      }
      return true;
    }
    if (event == "accepted") {
      const Json::Value jsep = message["jsep"];
      if (observer_ && jsep.isObject()) {
        observer_->OnJanusCallAccepted(jsep["sdp"].asString());
      }
      return true;
    }
    if (event == "hangup") {
      const std::string reason = payload["reason"].asString();
      if (observer_) {
        observer_->OnJanusHangup(reason);
      }
      return true;
    }
  } else if (janus_type == "trickle") {
    const Json::Value candidate = message["candidate"];
    if (observer_ && candidate.isObject()) {
      observer_->OnJanusRemoteTrickle(candidate);
    }
    return true;
  } else if (janus_type == "timeout") {
    RTC_LOG(LS_WARNING) << "Janus session timed out";
    if (observer_) {
      observer_->OnJanusError("Session timeout");
    }
    stop_polling_ = true;
    return false;
  }
  return true;
}

std::string JanusClient::BuildPath() const {
  return absl::StrFormat("/janus/%llu",
                         static_cast<unsigned long long>(session_id_));
}

std::string JanusClient::BuildPathWithHandle() const {
  return absl::StrFormat("/janus/%llu/%llu",
                         static_cast<unsigned long long>(session_id_),
                         static_cast<unsigned long long>(handle_id_));
}

std::string JanusClient::NewTransaction() const {
  return webrtc::CreateRandomString(12);
}

bool JanusClient::ValidateSuccessResponse(const Json::Value& response,
                                          const char* context,
                                          Json::Value* data_out) {
  if (!response.isObject()) {
    LogUnexpectedResponse(context, response);
    return false;
  }

  const Json::Value& janus_value = response["janus"];
  if (!janus_value.isString()) {
    LogUnexpectedResponse(context, response);
    return false;
  }

  if (janus_value.asString() != "success") {
    LogUnexpectedResponse(context, response);
    return false;
  }

  const Json::Value& data = response["data"];
  if (!data.isObject()) {
    LogUnexpectedResponse(context, response);
    return false;
  }
  if (data_out) {
    *data_out = data;
  }
  return true;
}

bool JanusClient::ExtractId(const Json::Value& data,
                            const char* context,
                            uint64_t* id_out) {
  if (!data.isObject()) {
    LogUnexpectedResponse(context, data);
    return false;
  }
  const Json::Value& id_value = data["id"];
  if (!id_value.isUInt64() && !id_value.isUInt() && !id_value.isInt64()) {
    LogUnexpectedResponse(context, data);
    return false;
  }
  *id_out = id_value.isUInt64() ? id_value.asUInt64()
                                : static_cast<uint64_t>(id_value.asLargestUInt());
  return true;
}

void JanusClient::LogUnexpectedResponse(const char* context,
                                        const Json::Value& response) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  RTC_LOG(LS_ERROR) << context << ": unexpected Janus response: "
                    << Json::writeString(writer, response);
}
