/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "p2p/base/p2p_transport_channel.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/functional/any_invocable.h"
#include "absl/memory/memory.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "api/array_view.h"
#include "api/async_dns_resolver.h"
#include "api/candidate.h"
#include "api/field_trials_view.h"
#include "api/ice_transport_interface.h"
#include "api/local_network_access_permission.h"
#include "api/rtc_error.h"
#include "api/sequence_checker.h"
#include "api/transport/enums.h"
#include "api/transport/stun.h"
#include "logging/rtc_event_log/events/rtc_event_ice_candidate_pair_config.h"
#include "logging/rtc_event_log/ice_logger.h"
#include "p2p/base/active_ice_controller_factory_interface.h"
#include "p2p/base/candidate_pair_interface.h"
#include "p2p/base/connection.h"
#include "p2p/base/connection_info.h"
#include "p2p/base/ice_controller_factory_interface.h"
#include "p2p/base/ice_switch_reason.h"
#include "p2p/base/ice_transport_internal.h"
#include "p2p/base/p2p_constants.h"
#include "p2p/base/port.h"
#include "p2p/base/port_allocator.h"
#include "p2p/base/port_interface.h"
#include "p2p/base/regathering_controller.h"
#include "p2p/base/transport_description.h"
#include "p2p/base/wrapping_active_ice_controller.h"
#include "p2p/dtls/dtls_stun_piggyback_callbacks.h"
#include "rtc_base/async_packet_socket.h"
#include "rtc_base/checks.h"
#include "rtc_base/dscp.h"
#include "rtc_base/experiments/struct_parameters_parser.h"
#include "rtc_base/ip_address.h"
#include "rtc_base/logging.h"
#include "rtc_base/net_helper.h"
#include "rtc_base/net_helpers.h"
#include "rtc_base/network.h"
#include "rtc_base/network/received_packet.h"
#include "rtc_base/network/sent_packet.h"
#include "rtc_base/network_constants.h"
#include "rtc_base/network_route.h"
#include "rtc_base/network_route_probe_bitrate_tracker.h"
#include "rtc_base/socket.h"
#include "rtc_base/socket_address.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"
#include "rtc_base/trace_event.h"
#include "system_wrappers/include/metrics.h"

namespace webrtc {
namespace {

PortInterface::CandidateOrigin GetOrigin(PortInterface* port,
                                         PortInterface* origin_port) {
  if (!origin_port)
    return PortInterface::ORIGIN_MESSAGE;
  else if (port == origin_port)
    return PortInterface::ORIGIN_THIS_PORT;
  else
    return PortInterface::ORIGIN_OTHER_PORT;
}

uint32_t GetWeakPingIntervalInFieldTrial(const FieldTrialsView* field_trials) {
  if (field_trials != nullptr) {
    uint32_t weak_ping_interval =
        ::strtoul(field_trials->Lookup("WebRTC-StunInterPacketDelay").c_str(),
                  nullptr, 10);
    if (weak_ping_interval) {
      return static_cast<int>(weak_ping_interval);
    }
  }
  return WEAK_PING_INTERVAL;
}

RouteEndpoint CreateRouteEndpointFromCandidate(bool local,
                                               const Candidate& candidate,
                                               bool uses_turn) {
  auto adapter_type = candidate.network_type();
  if (!local && adapter_type == ADAPTER_TYPE_UNKNOWN) {
    bool vpn;
    std::tie(adapter_type, vpn) =
        Network::GuessAdapterFromNetworkCost(candidate.network_cost());
  }

  // TODO(bugs.webrtc.org/9446) : Rewrite if information about remote network
  // adapter becomes available. The implication of this implementation is that
  // we will only ever report 1 adapter per type. In practice this is probably
  // fine, since the endpoint also contains network-id.
  uint16_t adapter_id = static_cast<int>(adapter_type);
  return RouteEndpoint(adapter_type, adapter_id, candidate.network_id(),
                       uses_turn);
}

// === Minimal multipath experiment knobs ===
// This is intentionally kept local to this .cc file for minimal footprint.
// Turn off by setting `kEnableIceMultipath` to false, or at runtime via
// webrtc::SetWebrtcMultipathSendMode(...) (see peerconnection_client
// `--webrtc_send_stack=` native|extended|round_robin|single_path_pacer).
constexpr bool kEnableIceMultipath = true;

bool IceMultipathDualPathSendActive() {
  return kEnableIceMultipath && IsWebrtcMultipathIceMultipathSendActive();
}
// Prefer an alternate connection on a different local network_id than the
// selected connection, when possible (useful for multi-NIC experiments).
constexpr bool kIceMultipathPreferDifferentNetwork = true;
// Only use multipath for packets that look like RTP/RTCP (SRTP/SRTCP). This
// avoids interfering with DTLS handshakes and other non-media traffic.
constexpr bool kIceMultipathOnlyRtpRtcp = true;
constexpr int64_t kMultipathScheduleWindowMs = 20;
constexpr int kMultipathMinScheduleWindowMs = 10;
constexpr int kMultipathMaxScheduleWindowMs = 20;
constexpr uint32_t kMultipathDefaultEstimatedBps = 300000;
constexpr size_t kMultipathMinBudgetBytes = 2400;
constexpr uint32_t kMultipathMinBudgetPackets = 2;
constexpr uint64_t kMultipathOptimismNumerator = 6;    // 1.2x
constexpr uint64_t kMultipathOptimismDenominator = 5;  // 1.2x
constexpr int kMultipathSchedulerLogIntervalPackets = 1000;
constexpr uint64_t kMultipathQuotaLogIntervalBatches = 50;
// Interval between periodic `RequestGccProbeOnNewPath()` calls while multipath
// send is active (refreshes per-path probe bitrate hints). Larger => fewer GCC
// probe clusters and less probing traffic.
constexpr int64_t kPeriodicProbeIntervalMs = 8000;
constexpr int kFrameIntervalIncreaseTriggerRatioPercent = 160;
constexpr int64_t kFrameIntervalIncreaseMinDeltaMs = 12;
constexpr int kFrameIntervalIncreaseConsecutiveFrames = 2;
constexpr int64_t kFrameIntervalResortMinIntervalMs = 1000;
constexpr int64_t kFrameIntervalEwmaNumerator = 7;
constexpr int64_t kFrameIntervalEwmaDenominator = 8;
constexpr int kNoProbeClusterId = -1;
constexpr int kPacketPriorityRetransmission = 0;
constexpr int kPacketPriorityAudio = 1;
constexpr int kPacketPriorityKeyFrame = 2;
constexpr int kPacketPriorityRedundancy = 3;
constexpr int kPacketPriorityOther = 4;

struct RtpFrameKey {
  uint32_t ssrc = 0;
  uint32_t timestamp = 0;
};

inline bool LooksLikeRtpOrRtcp(const char* data, size_t len) {
  if (data == nullptr || len < 2) {
    return false;
  }
  // RTP/RTCP packets have V=2 in the first two bits: 0b10xxxxxx.
  const uint8_t b0 = static_cast<uint8_t>(data[0]);
  return (b0 & 0xC0) == 0x80;
}

std::optional<RtpFrameKey> ParseRtpFrameKey(const char* data, size_t len) {
  if (!LooksLikeRtpOrRtcp(data, len) || len < 12) {
    return std::nullopt;
  }
  const uint8_t packet_type_byte = static_cast<uint8_t>(data[1]);
  // RTCP packet types occupy [192, 223]; only RTP carries media frame keys.
  if (packet_type_byte >= 192 && packet_type_byte <= 223) {
    return std::nullopt;
  }
  RtpFrameKey key;
  key.timestamp = (static_cast<uint32_t>(static_cast<uint8_t>(data[4])) << 24) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(data[5])) << 16) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(data[6])) << 8) |
                  static_cast<uint32_t>(static_cast<uint8_t>(data[7]));
  key.ssrc = (static_cast<uint32_t>(static_cast<uint8_t>(data[8])) << 24) |
             (static_cast<uint32_t>(static_cast<uint8_t>(data[9])) << 16) |
             (static_cast<uint32_t>(static_cast<uint8_t>(data[10])) << 8) |
             static_cast<uint32_t>(static_cast<uint8_t>(data[11]));
  return key;
}

}  // unnamed namespace

bool IceCredentialsChanged(absl::string_view old_ufrag,
                           absl::string_view old_pwd,
                           absl::string_view new_ufrag,
                           absl::string_view new_pwd) {
  // The standard (RFC 5245 Section 9.1.1.1) says that ICE restarts MUST change
  // both the ufrag and password. However, section 9.2.1.1 says changing the
  // ufrag OR password indicates an ICE restart. So, to keep compatibility with
  // endpoints that only change one, we'll treat this as an ICE restart.
  return (old_ufrag != new_ufrag) || (old_pwd != new_pwd);
}

std::unique_ptr<P2PTransportChannel> P2PTransportChannel::Create(
    absl::string_view transport_name,
    int component,
    IceTransportInit init) {
  return absl::WrapUnique(new P2PTransportChannel(
      transport_name, component, init.port_allocator(),
      init.async_dns_resolver_factory(), /*owned_dns_resolver_factory=*/nullptr,
      init.lna_permission_factory(), init.event_log(),
      init.ice_controller_factory(), init.active_ice_controller_factory(),
      init.field_trials()));
}

P2PTransportChannel::P2PTransportChannel(absl::string_view transport_name,
                                         int component,
                                         PortAllocator* allocator,
                                         const FieldTrialsView* field_trials)
    : P2PTransportChannel(transport_name,
                          component,
                          allocator,
                          /* async_dns_resolver_factory= */ nullptr,
                          /* owned_dns_resolver_factory= */ nullptr,
                          /* lna_permission_factory= */ nullptr,
                          /* event_log= */ nullptr,
                          /* ice_controller_factory= */ nullptr,
                          /* active_ice_controller_factory= */ nullptr,
                          field_trials) {}

// Private constructor, called from Create()
P2PTransportChannel::P2PTransportChannel(
    absl::string_view transport_name,
    int component,
    PortAllocator* allocator,
    AsyncDnsResolverFactoryInterface* async_dns_resolver_factory,
    std::unique_ptr<AsyncDnsResolverFactoryInterface>
        owned_dns_resolver_factory,
    LocalNetworkAccessPermissionFactoryInterface* lna_permission_factory,
    RtcEventLog* event_log,
    IceControllerFactoryInterface* ice_controller_factory,
    ActiveIceControllerFactoryInterface* active_ice_controller_factory,
    const FieldTrialsView* field_trials)
    : transport_name_(transport_name),
      component_(component),
      allocator_(allocator),
      // If owned_dns_resolver_factory is given, async_dns_resolver_factory is
      // ignored.
      async_dns_resolver_factory_(owned_dns_resolver_factory
                                      ? owned_dns_resolver_factory.get()
                                      : async_dns_resolver_factory),
      owned_dns_resolver_factory_(std::move(owned_dns_resolver_factory)),
      lna_permission_factory_(lna_permission_factory),
      network_thread_(Thread::Current()),
      incoming_only_(false),
      error_(0),
      remote_ice_mode_(ICEMODE_FULL),
      ice_role_(ICEROLE_UNKNOWN),
      gathering_state_(kIceGatheringNew),
      weak_ping_interval_(GetWeakPingIntervalInFieldTrial(field_trials)),
      config_(RECEIVING_TIMEOUT,
              BACKUP_CONNECTION_PING_INTERVAL,
              GATHER_ONCE /* continual_gathering_policy */,
              false /* prioritize_most_likely_candidate_pairs */,
              STRONG_AND_STABLE_WRITABLE_CONNECTION_PING_INTERVAL,
              true /* presume_writable_when_fully_relayed */,
              REGATHER_ON_FAILED_NETWORKS_INTERVAL,
              RECEIVING_SWITCHING_DELAY),
      field_trials_(field_trials) {
  TRACE_EVENT0("webrtc", "P2PTransportChannel::P2PTransportChannel");
  RTC_DCHECK(allocator_ != nullptr);
  RTC_DCHECK(!transport_name_.empty());
  // Validate IceConfig even for mostly built-in constant default values in case
  // we change them.
  RTC_DCHECK(config_.IsValid().ok());
  BasicRegatheringController::Config regathering_config;
  regathering_config.regather_on_failed_networks_interval =
      config_.regather_on_failed_networks_interval_or_default();
  regathering_controller_ = std::make_unique<BasicRegatheringController>(
      regathering_config, this, network_thread_);
  // We populate the change in the candidate filter to the session taken by
  // the transport.
  allocator_->SignalCandidateFilterChanged.connect(
      this, &P2PTransportChannel::OnCandidateFilterChanged);
  ice_event_log_.set_event_log(event_log);

  ParseFieldTrials(field_trials);

  IceControllerFactoryArgs args{
      [this] { return GetState(); }, [this] { return GetIceRole(); },
      [this](const Connection* connection) {
        return IsPortPruned(connection->port()) ||
               IsRemoteCandidatePruned(connection->remote_candidate());
      },
      &ice_field_trials_,
      field_trials ? field_trials->Lookup("WebRTC-IceControllerFieldTrials")
                   : ""};

  if (active_ice_controller_factory) {
    ActiveIceControllerFactoryArgs active_args{args,
                                               /* ice_agent= */ this};
    ice_controller_ = active_ice_controller_factory->Create(active_args);
  } else {
    ice_controller_ = std::make_unique<WrappingActiveIceController>(
        /* ice_agent= */ this, ice_controller_factory, args);
  }
}

P2PTransportChannel::~P2PTransportChannel() {
  TRACE_EVENT0("webrtc", "P2PTransportChannel::~P2PTransportChannel");
  RTC_DCHECK_RUN_ON(network_thread_);
  std::vector<Connection*> copy(connections_.begin(), connections_.end());
  for (Connection* connection : copy) {
    connection->SignalDestroyed.disconnect(this);
    RemoveConnection(connection);
    connection->Destroy();
  }
  resolvers_.clear();
}

// Add the allocator session to our list so that we know which sessions
// are still active.
void P2PTransportChannel::AddAllocatorSession(
    std::unique_ptr<PortAllocatorSession> session) {
  RTC_DCHECK_RUN_ON(network_thread_);

  session->set_generation(static_cast<uint32_t>(allocator_sessions_.size()));
  session->SignalPortReady.connect(this, &P2PTransportChannel::OnPortReady);
  session->SignalPortsPruned.connect(this, &P2PTransportChannel::OnPortsPruned);
  session->SignalCandidatesReady.connect(
      this, &P2PTransportChannel::OnCandidatesReady);
  session->SignalCandidateError.connect(this,
                                        &P2PTransportChannel::OnCandidateError);
  session->SignalCandidatesRemoved.connect(
      this, &P2PTransportChannel::OnCandidatesRemoved);
  session->SignalCandidatesAllocationDone.connect(
      this, &P2PTransportChannel::OnCandidatesAllocationDone);
  if (!allocator_sessions_.empty()) {
    allocator_session()->PruneAllPorts();
  }
  allocator_sessions_.push_back(std::move(session));
  regathering_controller_->set_allocator_session(allocator_session());

  // We now only want to apply new candidates that we receive to the ports
  // created by this new session because these are replacing those of the
  // previous sessions.
  PruneAllPorts();
}

void P2PTransportChannel::AddConnection(Connection* connection) {
  RTC_DCHECK_RUN_ON(network_thread_);
  connection->set_receiving_timeout(config_.receiving_timeout);
  connection->set_unwritable_timeout(config_.ice_unwritable_timeout);
  connection->set_unwritable_min_checks(config_.ice_unwritable_min_checks);
  connection->set_inactive_timeout(config_.ice_inactive_timeout);
  connection->RegisterReceivedPacketCallback(
      [&](Connection* connection, const ReceivedIpPacket& packet) {
        OnReadPacket(connection, packet);
      });
  connection->SignalReadyToSend.connect(this,
                                        &P2PTransportChannel::OnReadyToSend);
  connection->SignalStateChange.connect(
      this, &P2PTransportChannel::OnConnectionStateChange);
  connection->SignalDestroyed.connect(
      this, &P2PTransportChannel::OnConnectionDestroyed);
  connection->SignalNominated.connect(this, &P2PTransportChannel::OnNominated);

  had_connection_ = true;

  connection->set_ice_event_log(&ice_event_log_);
  connection->SetIceFieldTrials(&ice_field_trials_);
  connection->SetStunDictConsumer(
      [this](const StunByteStringAttribute* delta) {
        return GoogDeltaReceived(delta);
      },
      [this](RTCErrorOr<const StunUInt64Attribute*> delta_ack) {
        GoogDeltaAckReceived(std::move(delta_ack));
      });
  if (!dtls_stun_piggyback_callbacks_.empty()) {
    connection->RegisterDtlsPiggyback(DtlsStunPiggybackCallbacks(
        [&](auto request) {
          return dtls_stun_piggyback_callbacks_.send_data(request);
        },
        [&](auto data, auto ack) {
          dtls_stun_piggyback_callbacks_.recv_data(data, ack);
        }));
  }

  LogCandidatePairConfig(connection, IceCandidatePairConfigType::kAdded);

  connections_.push_back(connection);
  ice_controller_->OnConnectionAdded(connection);
}

void P2PTransportChannel::ForgetLearnedStateForConnections(
    ArrayView<const Connection* const> connections) {
  for (const Connection* con : connections) {
    FromIceController(con)->ForgetLearnedState();
  }
}

void P2PTransportChannel::SetIceRole(IceRole ice_role) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (ice_role_ != ice_role) {
    ice_role_ = ice_role;
    for (PortInterface* port : ports_) {
      port->SetIceRole(ice_role);
    }
    // Update role on pruned ports as well, because they may still have
    // connections alive that should be using the correct role.
    for (PortInterface* port : pruned_ports_) {
      port->SetIceRole(ice_role);
    }
  }
}

IceRole P2PTransportChannel::GetIceRole() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return ice_role_;
}

IceTransportStateInternal P2PTransportChannel::GetState() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return state_;
}

IceTransportState P2PTransportChannel::GetIceTransportState() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return standardized_state_;
}

const std::string& P2PTransportChannel::transport_name() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return transport_name_;
}

int P2PTransportChannel::component() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return component_;
}

bool P2PTransportChannel::writable() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return writable_;
}

bool P2PTransportChannel::receiving() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return receiving_;
}

IceGatheringState P2PTransportChannel::gathering_state() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return gathering_state_;
}

std::optional<int> P2PTransportChannel::GetRttEstimate() {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (selected_connection_ != nullptr &&
      selected_connection_->rtt_samples() > 0) {
    return selected_connection_->rtt();
  } else {
    return std::nullopt;
  }
}

std::optional<const CandidatePair>
P2PTransportChannel::GetSelectedCandidatePair() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (selected_connection_ == nullptr) {
    return std::nullopt;
  }

  CandidatePair pair;
  pair.local = SanitizeLocalCandidate(selected_connection_->local_candidate());
  pair.remote =
      SanitizeRemoteCandidate(selected_connection_->remote_candidate());
  return pair;
}

// A channel is considered ICE completed once there is at most one active
// connection per network and at least one active connection.
IceTransportStateInternal P2PTransportChannel::ComputeState() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (!had_connection_) {
    return IceTransportStateInternal::STATE_INIT;
  }

  std::vector<Connection*> active_connections;
  for (Connection* connection : connections_) {
    if (connection->active()) {
      active_connections.push_back(connection);
    }
  }
  if (active_connections.empty()) {
    return IceTransportStateInternal::STATE_FAILED;
  }

  std::set<const Network*> networks;
  for (Connection* connection : active_connections) {
    const Network* network = connection->network();
    if (networks.find(network) == networks.end()) {
      networks.insert(network);
    } else {
      RTC_LOG(LS_VERBOSE) << ToString()
                          << ": Ice not completed yet for this channel as "
                          << network->ToString()
                          << " has more than 1 connection.";
      return IceTransportStateInternal::STATE_CONNECTING;
    }
  }

  ice_event_log_.DumpCandidatePairDescriptionToMemoryAsConfigEvents();
  return IceTransportStateInternal::STATE_COMPLETED;
}

// Compute the current RTCIceTransportState as described in
// https://www.w3.org/TR/webrtc/#dom-rtcicetransportstate
// TODO(bugs.webrtc.org/9218): Start signaling kCompleted once we have
// implemented end-of-candidates signalling.
IceTransportState P2PTransportChannel::ComputeIceTransportState() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  bool has_connection = false;
  for (Connection* connection : connections_) {
    if (connection->active()) {
      has_connection = true;
      break;
    }
  }

  if (had_connection_ && !has_connection) {
    return IceTransportState::kFailed;
  }

  if (!writable() && has_been_writable_) {
    return IceTransportState::kDisconnected;
  }

  if (!had_connection_ && !has_connection) {
    return IceTransportState::kNew;
  }

  if (has_connection && !writable()) {
    // A candidate pair has been formed by adding a remote candidate
    // and gathering a local candidate.
    return IceTransportState::kChecking;
  }

  return IceTransportState::kConnected;
}

void P2PTransportChannel::SetIceParameters(const IceParameters& ice_params) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_LOG(LS_INFO) << "Set ICE ufrag: " << ice_params.ufrag
                   << " pwd: " << ice_params.pwd << " on transport "
                   << transport_name();
  ice_parameters_ = ice_params;
  // Note: Candidate gathering will restart when MaybeStartGathering is next
  // called.
}

void P2PTransportChannel::SetRemoteIceParameters(
    const IceParameters& ice_params) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_LOG(LS_INFO) << "Received remote ICE parameters: ufrag="
                   << ice_params.ufrag << ", renomination "
                   << (ice_params.renomination ? "enabled" : "disabled");
  const IceParameters* current_ice = remote_ice_parameters();
  if (!current_ice || *current_ice != ice_params) {
    // Keep the ICE credentials so that newer connections
    // are prioritized over the older ones.
    remote_ice_parameters_.push_back(ice_params);
  }

  // Update the pwd of remote candidate if needed.
  for (RemoteCandidate& candidate : remote_candidates_) {
    if (candidate.username() == ice_params.ufrag &&
        candidate.password().empty()) {
      candidate.set_password(ice_params.pwd);
    }
  }
  // We need to update the credentials and generation for any peer reflexive
  // candidates.
  for (Connection* conn : connections_) {
    conn->MaybeSetRemoteIceParametersAndGeneration(
        ice_params, static_cast<int>(remote_ice_parameters_.size() - 1));
  }
  // Updating the remote ICE candidate generation could change the sort order.
  ice_controller_->OnSortAndSwitchRequest(
      IceSwitchReason::REMOTE_CANDIDATE_GENERATION_CHANGE);
}

void P2PTransportChannel::SetRemoteIceMode(IceMode mode) {
  RTC_DCHECK_RUN_ON(network_thread_);
  remote_ice_mode_ = mode;
}

// TODO(qingsi): We apply the convention that setting a std::optional parameter
// to null restores its default value in the implementation. However, some
// std::optional parameters are only processed below if non-null, e.g.,
// regather_on_failed_networks_interval, and thus there is no way to restore the
// defaults. Fix this issue later for consistency.
void P2PTransportChannel::SetIceConfig(const IceConfig& config) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (config_.continual_gathering_policy != config.continual_gathering_policy) {
    if (!allocator_sessions_.empty()) {
      RTC_LOG(LS_ERROR) << "Trying to change continual gathering policy "
                           "when gathering has already started!";
    } else {
      config_.continual_gathering_policy = config.continual_gathering_policy;
      RTC_LOG(LS_INFO) << "Set continual_gathering_policy to "
                       << config_.continual_gathering_policy;
    }
  }

  if (config_.backup_connection_ping_interval !=
      config.backup_connection_ping_interval) {
    config_.backup_connection_ping_interval =
        config.backup_connection_ping_interval;
    RTC_LOG(LS_INFO) << "Set backup connection ping interval to "
                     << config_.backup_connection_ping_interval_or_default()
                     << " milliseconds.";
  }
  if (config_.receiving_timeout != config.receiving_timeout) {
    config_.receiving_timeout = config.receiving_timeout;
    for (Connection* connection : connections_) {
      connection->set_receiving_timeout(config_.receiving_timeout);
    }
    RTC_LOG(LS_INFO) << "Set ICE receiving timeout to "
                     << config_.receiving_timeout_or_default()
                     << " milliseconds";
  }

  config_.prioritize_most_likely_candidate_pairs =
      config.prioritize_most_likely_candidate_pairs;
  RTC_LOG(LS_INFO) << "Set ping most likely connection to "
                   << config_.prioritize_most_likely_candidate_pairs;

  if (config_.stable_writable_connection_ping_interval !=
      config.stable_writable_connection_ping_interval) {
    config_.stable_writable_connection_ping_interval =
        config.stable_writable_connection_ping_interval;
    RTC_LOG(LS_INFO)
        << "Set stable_writable_connection_ping_interval to "
        << config_.stable_writable_connection_ping_interval_or_default();
  }

  if (config_.presume_writable_when_fully_relayed !=
      config.presume_writable_when_fully_relayed) {
    if (!connections_.empty()) {
      RTC_LOG(LS_ERROR) << "Trying to change 'presume writable' "
                           "while connections already exist!";
    } else {
      config_.presume_writable_when_fully_relayed =
          config.presume_writable_when_fully_relayed;
      RTC_LOG(LS_INFO) << "Set presume writable when fully relayed to "
                       << config_.presume_writable_when_fully_relayed;
    }
  }

  config_.surface_ice_candidates_on_ice_transport_type_changed =
      config.surface_ice_candidates_on_ice_transport_type_changed;
  if (config_.surface_ice_candidates_on_ice_transport_type_changed &&
      config_.continual_gathering_policy != GATHER_CONTINUALLY) {
    RTC_LOG(LS_WARNING)
        << "surface_ice_candidates_on_ice_transport_type_changed is "
           "ineffective since we do not gather continually.";
  }

  if (config_.regather_on_failed_networks_interval !=
      config.regather_on_failed_networks_interval) {
    config_.regather_on_failed_networks_interval =
        config.regather_on_failed_networks_interval;
    RTC_LOG(LS_INFO)
        << "Set regather_on_failed_networks_interval to "
        << config_.regather_on_failed_networks_interval_or_default();
  }

  if (config_.receiving_switching_delay != config.receiving_switching_delay) {
    config_.receiving_switching_delay = config.receiving_switching_delay;
    RTC_LOG(LS_INFO) << "Set receiving_switching_delay to "
                     << config_.receiving_switching_delay_or_default();
  }

  if (config_.default_nomination_mode != config.default_nomination_mode) {
    config_.default_nomination_mode = config.default_nomination_mode;
    RTC_LOG(LS_INFO) << "Set default nomination mode to "
                     << static_cast<int>(config_.default_nomination_mode);
  }

  if (config_.ice_check_interval_strong_connectivity !=
      config.ice_check_interval_strong_connectivity) {
    config_.ice_check_interval_strong_connectivity =
        config.ice_check_interval_strong_connectivity;
    RTC_LOG(LS_INFO)
        << "Set strong ping interval to "
        << config_.ice_check_interval_strong_connectivity_or_default();
  }

  if (config_.ice_check_interval_weak_connectivity !=
      config.ice_check_interval_weak_connectivity) {
    config_.ice_check_interval_weak_connectivity =
        config.ice_check_interval_weak_connectivity;
    RTC_LOG(LS_INFO)
        << "Set weak ping interval to "
        << config_.ice_check_interval_weak_connectivity_or_default();
  }

  if (config_.ice_check_min_interval != config.ice_check_min_interval) {
    config_.ice_check_min_interval = config.ice_check_min_interval;
    RTC_LOG(LS_INFO) << "Set min ping interval to "
                     << config_.ice_check_min_interval_or_default();
  }

  if (config_.ice_unwritable_timeout != config.ice_unwritable_timeout) {
    config_.ice_unwritable_timeout = config.ice_unwritable_timeout;
    for (Connection* conn : connections_) {
      conn->set_unwritable_timeout(config_.ice_unwritable_timeout);
    }
    RTC_LOG(LS_INFO) << "Set unwritable timeout to "
                     << config_.ice_unwritable_timeout_or_default();
  }

  if (config_.ice_unwritable_min_checks != config.ice_unwritable_min_checks) {
    config_.ice_unwritable_min_checks = config.ice_unwritable_min_checks;
    for (Connection* conn : connections_) {
      conn->set_unwritable_min_checks(config_.ice_unwritable_min_checks);
    }
    RTC_LOG(LS_INFO) << "Set unwritable min checks to "
                     << config_.ice_unwritable_min_checks_or_default();
  }

  if (config_.ice_inactive_timeout != config.ice_inactive_timeout) {
    config_.ice_inactive_timeout = config.ice_inactive_timeout;
    for (Connection* conn : connections_) {
      conn->set_inactive_timeout(config_.ice_inactive_timeout);
    }
    RTC_LOG(LS_INFO) << "Set inactive timeout to "
                     << config_.ice_inactive_timeout_or_default();
  }

  if (config_.network_preference != config.network_preference) {
    config_.network_preference = config.network_preference;
    ice_controller_->OnSortAndSwitchRequest(
        IceSwitchReason::NETWORK_PREFERENCE_CHANGE);
    RTC_LOG(LS_INFO) << "Set network preference to "
                     << (config_.network_preference.has_value()
                             ? config_.network_preference.value()
                             : -1);  // network_preference cannot be bound to
                                     // int with value_or.
  }

  // TODO(qingsi): Resolve the naming conflict of stun_keepalive_delay in
  // UDPPort and stun_keepalive_interval.
  if (config_.stun_keepalive_interval != config.stun_keepalive_interval) {
    config_.stun_keepalive_interval = config.stun_keepalive_interval;
    allocator_session()->SetStunKeepaliveIntervalForReadyPorts(
        config_.stun_keepalive_interval);
    RTC_LOG(LS_INFO) << "Set STUN keepalive interval to "
                     << config.stun_keepalive_interval_or_default();
  }

  BasicRegatheringController::Config regathering_config;
  regathering_config.regather_on_failed_networks_interval =
      config_.regather_on_failed_networks_interval_or_default();
  regathering_controller_->SetConfig(regathering_config);

  config_.vpn_preference = config.vpn_preference;
  allocator_->SetVpnPreference(config_.vpn_preference);

  ice_controller_->SetIceConfig(config_);
  if (config_.dtls_handshake_in_stun != config.dtls_handshake_in_stun) {
    config_.dtls_handshake_in_stun = config.dtls_handshake_in_stun;
    RTC_LOG(LS_INFO) << "Set DTLS handshake in STUN to "
                     << config.dtls_handshake_in_stun;
  }

  RTC_DCHECK(config_.IsValid().ok());
}

void P2PTransportChannel::ParseFieldTrials(
    const FieldTrialsView* field_trials) {
  if (field_trials == nullptr) {
    return;
  }

  if (field_trials->IsEnabled("WebRTC-ExtraICEPing")) {
    RTC_LOG(LS_INFO) << "Set WebRTC-ExtraICEPing: Enabled";
  }

  StructParametersParser::Create(
      // go/skylift-light
      "skip_relay_to_non_relay_connections",
      &ice_field_trials_.skip_relay_to_non_relay_connections,
      // Limiting pings sent.
      "max_outstanding_pings", &ice_field_trials_.max_outstanding_pings,
      // Delay initial selection of connection.
      "initial_select_dampening", &ice_field_trials_.initial_select_dampening,
      // Delay initial selection of connections, that are receiving.
      "initial_select_dampening_ping_received",
      &ice_field_trials_.initial_select_dampening_ping_received,
      // Reply that we support goog ping.
      "announce_goog_ping", &ice_field_trials_.announce_goog_ping,
      // Use goog ping if remote support it.
      "enable_goog_ping", &ice_field_trials_.enable_goog_ping,
      // How fast does a RTT sample decay.
      "rtt_estimate_halftime_ms", &ice_field_trials_.rtt_estimate_halftime_ms,
      // Make sure that nomination reaching ICE controlled asap.
      "send_ping_on_switch_ice_controlling",
      &ice_field_trials_.send_ping_on_switch_ice_controlling,
      // Make sure that nomination reaching ICE controlled asap.
      "send_ping_on_selected_ice_controlling",
      &ice_field_trials_.send_ping_on_selected_ice_controlling,
      // Reply to nomination ASAP.
      "send_ping_on_nomination_ice_controlled",
      &ice_field_trials_.send_ping_on_nomination_ice_controlled,
      // Allow connections to live untouched longer that 30s.
      "dead_connection_timeout_ms",
      &ice_field_trials_.dead_connection_timeout_ms,
      // Stop gathering on strongly connected.
      "stop_gather_on_strongly_connected",
      &ice_field_trials_.stop_gather_on_strongly_connected,
      // GOOG_DELTA
      "enable_goog_delta", &ice_field_trials_.enable_goog_delta,
      "answer_goog_delta", &ice_field_trials_.answer_goog_delta)
      ->Parse(field_trials->Lookup("WebRTC-IceFieldTrials"));

  if (ice_field_trials_.dead_connection_timeout_ms < 30000) {
    RTC_LOG(LS_WARNING) << "dead_connection_timeout_ms set to "
                        << ice_field_trials_.dead_connection_timeout_ms
                        << " increasing it to 30000";
    ice_field_trials_.dead_connection_timeout_ms = 30000;
  }

  if (ice_field_trials_.skip_relay_to_non_relay_connections) {
    RTC_LOG(LS_INFO) << "Set skip_relay_to_non_relay_connections";
  }

  if (ice_field_trials_.max_outstanding_pings.has_value()) {
    RTC_LOG(LS_INFO) << "Set max_outstanding_pings: "
                     << *ice_field_trials_.max_outstanding_pings;
  }

  if (ice_field_trials_.initial_select_dampening.has_value()) {
    RTC_LOG(LS_INFO) << "Set initial_select_dampening: "
                     << *ice_field_trials_.initial_select_dampening;
  }

  if (ice_field_trials_.initial_select_dampening_ping_received.has_value()) {
    RTC_LOG(LS_INFO)
        << "Set initial_select_dampening_ping_received: "
        << *ice_field_trials_.initial_select_dampening_ping_received;
  }

  // DSCP override, allow user to specify (any) int value
  // that will be used for tagging all packets.
  StructParametersParser::Create("override_dscp",
                                 &ice_field_trials_.override_dscp)
      ->Parse(field_trials->Lookup("WebRTC-DscpFieldTrial"));

  if (ice_field_trials_.override_dscp) {
    SetOption(Socket::OPT_DSCP, *ice_field_trials_.override_dscp);
  }

  std::string field_trial_string =
      field_trials->Lookup("WebRTC-SetSocketReceiveBuffer");
  int receive_buffer_size_kb = 0;
  sscanf(field_trial_string.c_str(), "Enabled-%d", &receive_buffer_size_kb);
  if (receive_buffer_size_kb > 0) {
    RTC_LOG(LS_INFO) << "Set WebRTC-SetSocketReceiveBuffer: Enabled and set to "
                     << receive_buffer_size_kb << "kb";
    SetOption(Socket::OPT_RCVBUF, receive_buffer_size_kb * 1024);
  }

  ice_field_trials_.piggyback_ice_check_acknowledgement =
      field_trials->IsEnabled("WebRTC-PiggybackIceCheckAcknowledgement");

  ice_field_trials_.extra_ice_ping =
      field_trials->IsEnabled("WebRTC-ExtraICEPing");

  if (!ice_field_trials_.enable_goog_delta) {
    stun_dict_writer_.Disable();
  }

  if (field_trials->IsEnabled("WebRTC-RFC8888CongestionControlFeedback")) {
    int desired_recv_esn = 1;
    RTC_LOG(LS_INFO) << "Set WebRTC-RFC8888CongestionControlFeedback: Enable "
                        "and set ECN recving mode";
    SetOption(Socket::OPT_RECV_ECN, desired_recv_esn);
  }
}

const IceConfig& P2PTransportChannel::config() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return config_;
}

const Connection* P2PTransportChannel::selected_connection() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return selected_connection_;
}

int P2PTransportChannel::check_receiving_interval() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return std::max(MIN_CHECK_RECEIVING_INTERVAL,
                  config_.receiving_timeout_or_default() / 10);
}

void P2PTransportChannel::MaybeStartGathering() {
  RTC_DCHECK_RUN_ON(network_thread_);
  // TODO(bugs.webrtc.org/14605): ensure tie_breaker_ is set.
  if (ice_parameters_.ufrag.empty() || ice_parameters_.pwd.empty()) {
    RTC_LOG(LS_ERROR)
        << "Cannot gather candidates because ICE parameters are empty"
           " ufrag: "
        << ice_parameters_.ufrag << " pwd: " << ice_parameters_.pwd;
    return;
  }
  // Start gathering if we never started before, or if an ICE restart occurred.
  if (allocator_sessions_.empty() ||
      IceCredentialsChanged(allocator_sessions_.back()->ice_ufrag(),
                            allocator_sessions_.back()->ice_pwd(),
                            ice_parameters_.ufrag, ice_parameters_.pwd)) {
    if (gathering_state_ != kIceGatheringGathering) {
      gathering_state_ = kIceGatheringGathering;
      SendGatheringStateEvent();
    }

    for (const auto& session : allocator_sessions_) {
      if (session->IsStopped()) {
        continue;
      }
      session->StopGettingPorts();
    }

    // Time for a new allocator.
    std::unique_ptr<PortAllocatorSession> pooled_session =
        allocator_->TakePooledSession(transport_name(), component(),
                                      ice_parameters_.ufrag,
                                      ice_parameters_.pwd);
    if (pooled_session) {
      AddAllocatorSession(std::move(pooled_session));
      PortAllocatorSession* raw_pooled_session =
          allocator_sessions_.back().get();
      // Process the pooled session's existing candidates/ports, if they exist.
      OnCandidatesReady(raw_pooled_session,
                        raw_pooled_session->ReadyCandidates());
      for (PortInterface* port : allocator_sessions_.back()->ReadyPorts()) {
        OnPortReady(raw_pooled_session, port);
      }
      if (allocator_sessions_.back()->CandidatesAllocationDone()) {
        OnCandidatesAllocationDone(raw_pooled_session);
      }
    } else {
      AddAllocatorSession(allocator_->CreateSession(
          transport_name(), component(), ice_parameters_.ufrag,
          ice_parameters_.pwd));
      allocator_sessions_.back()->StartGettingPorts();
    }
  }
}

// A new port is available, attempt to make connections for it
void P2PTransportChannel::OnPortReady(PortAllocatorSession* /* session */,
                                      PortInterface* port) {
  RTC_DCHECK_RUN_ON(network_thread_);

  // Set in-effect options on the new port
  for (OptionMap::const_iterator it = options_.begin(); it != options_.end();
       ++it) {
    int val = port->SetOption(it->first, it->second);
    if (val < 0) {
      // Errors are frequent, so use LS_INFO. bugs.webrtc.org/9221
      RTC_LOG(LS_INFO) << port->ToString() << ": SetOption(" << it->first
                       << ", " << it->second
                       << ") failed: " << port->GetError();
    }
  }

  // Remember the ports and candidates, and signal that candidates are ready.
  // The session will handle this, and send an initiate/accept/modify message
  // if one is pending.

  port->SetIceRole(ice_role_);
  port->SetIceTiebreaker(allocator_->ice_tiebreaker());
  ports_.push_back(port);
  port->SignalUnknownAddress.connect(this,
                                     &P2PTransportChannel::OnUnknownAddress);
  port->SubscribePortDestroyed(
      [this](PortInterface* port) { OnPortDestroyed(port); });

  port->SignalRoleConflict.connect(this, &P2PTransportChannel::OnRoleConflict);
  port->SignalSentPacket.connect(this, &P2PTransportChannel::OnSentPacket);

  // Attempt to create a connection from this new port to all of the remote
  // candidates that we were given so far.

  std::vector<RemoteCandidate>::iterator iter;
  for (iter = remote_candidates_.begin(); iter != remote_candidates_.end();
       ++iter) {
    CreateConnection(port, *iter, iter->origin_port());
  }

  ice_controller_->OnImmediateSortAndSwitchRequest(
      IceSwitchReason::NEW_CONNECTION_FROM_LOCAL_CANDIDATE);
}

// A new candidate is available, let listeners know
void P2PTransportChannel::OnCandidatesReady(
    PortAllocatorSession* /* session */,
    const std::vector<Candidate>& candidates) {
  RTC_DCHECK_RUN_ON(network_thread_);
  for (size_t i = 0; i < candidates.size(); ++i) {
    SignalCandidateGathered(this, candidates[i]);
  }
}

void P2PTransportChannel::OnCandidateError(
    PortAllocatorSession* /* session */,
    const IceCandidateErrorEvent& event) {
  RTC_DCHECK(network_thread_ == Thread::Current());
  if (candidate_error_callback_) {
    candidate_error_callback_(this, event);
  }
}

void P2PTransportChannel::OnCandidatesAllocationDone(
    PortAllocatorSession* /* session */) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (config_.gather_continually()) {
    RTC_LOG(LS_INFO) << "P2PTransportChannel: " << transport_name()
                     << ", component " << component()
                     << " gathering complete, but using continual "
                        "gathering so not changing gathering state.";
    return;
  }
  gathering_state_ = kIceGatheringComplete;
  RTC_LOG(LS_INFO) << "P2PTransportChannel: " << transport_name()
                   << ", component " << component() << " gathering complete";
  SendGatheringStateEvent();
}

// Handle stun packets
void P2PTransportChannel::OnUnknownAddress(PortInterface* port,
                                           const SocketAddress& address,
                                           ProtocolType proto,
                                           IceMessage* stun_msg,
                                           const std::string& remote_username,
                                           bool port_muxed) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_DCHECK(stun_msg);

  // Port has received a valid stun packet from an address that no Connection
  // is currently available for. See if we already have a candidate with the
  // address. If it isn't we need to create new candidate for it.
  //
  // TODO(qingsi): There is a caveat of the logic below if we have remote
  // candidates with hostnames. We could create a prflx candidate that is
  // identical to a host candidate that are currently in the process of name
  // resolution. We would not have a duplicate candidate since when adding the
  // resolved host candidate, FinishingAddingRemoteCandidate does
  // MaybeUpdatePeerReflexiveCandidate, and the prflx candidate would be updated
  // to a host candidate. As a result, for a brief moment we would have a prflx
  // candidate showing a private IP address, though we do not signal prflx
  // candidates to applications and we could obfuscate the IP addresses of prflx
  // candidates in P2PTransportChannel::GetStats. The difficulty of preventing
  // creating the prflx from the beginning is that we do not have a reliable way
  // to claim two candidates are identical without the address information. If
  // we always pause the addition of a prflx candidate when there is ongoing
  // name resolution and dedup after we have a resolved address, we run into the
  // risk of losing/delaying the addition of a non-identical candidate that
  // could be the only way to have a connection, if the resolution never
  // completes or is significantly delayed.
  const Candidate* candidate = nullptr;
  for (const Candidate& c : remote_candidates_) {
    if (c.username() == remote_username && c.address() == address &&
        c.protocol() == ProtoToString(proto)) {
      candidate = &c;
      break;
    }
  }

  uint32_t remote_generation = 0;
  std::string remote_password;
  // The STUN binding request may arrive after setRemoteDescription and before
  // adding remote candidate, so we need to set the password to the shared
  // password and set the generation if the user name matches.
  const IceParameters* ice_param =
      FindRemoteIceFromUfrag(remote_username, &remote_generation);
  // Note: if not found, the remote_generation will still be 0.
  if (ice_param != nullptr) {
    remote_password = ice_param->pwd;
  }

  Candidate remote_candidate;
  bool remote_candidate_is_new = (candidate == nullptr);
  if (!remote_candidate_is_new) {
    remote_candidate = *candidate;
  } else {
    // Create a new candidate with this address.
    // The priority of the candidate is set to the PRIORITY attribute
    // from the request.
    const StunUInt32Attribute* priority_attr =
        stun_msg->GetUInt32(STUN_ATTR_PRIORITY);
    if (!priority_attr) {
      RTC_LOG(LS_WARNING) << "P2PTransportChannel::OnUnknownAddress - "
                             "No STUN_ATTR_PRIORITY found in the "
                             "stun request message";
      port->SendBindingErrorResponse(stun_msg, address, STUN_ERROR_BAD_REQUEST,
                                     STUN_ERROR_REASON_BAD_REQUEST);
      return;
    }
    int remote_candidate_priority = priority_attr->value();

    uint16_t network_id = 0;
    uint16_t network_cost = 0;
    const StunUInt32Attribute* network_attr =
        stun_msg->GetUInt32(STUN_ATTR_GOOG_NETWORK_INFO);
    if (network_attr) {
      uint32_t network_info = network_attr->value();
      network_id = static_cast<uint16_t>(network_info >> 16);
      network_cost = static_cast<uint16_t>(network_info);
    }

    // RFC 5245
    // If the source transport address of the request does not match any
    // existing remote candidates, it represents a new peer reflexive remote
    // candidate.
    remote_candidate = Candidate(
        component(), ProtoToString(proto), address, remote_candidate_priority,
        remote_username, remote_password, IceCandidateType::kPrflx,
        remote_generation, "", network_id, network_cost);
    if (proto == PROTO_TCP) {
      remote_candidate.set_tcptype(TCPTYPE_ACTIVE_STR);
    }

    // From RFC 5245, section-7.2.1.3:
    // The foundation of the candidate is set to an arbitrary value, different
    // from the foundation for all other remote candidates.
    remote_candidate.ComputePrflxFoundation();
  }

  // RFC5245, the agent constructs a pair whose local candidate is equal to
  // the transport address on which the STUN request was received, and a
  // remote candidate equal to the source transport address where the
  // request came from.

  // There shouldn't be an existing connection with this remote address.
  // When ports are muxed, this channel might get multiple unknown address
  // signals. In that case if the connection is already exists, we should
  // simply ignore the signal otherwise send server error.
  if (port->GetConnection(remote_candidate.address())) {
    if (port_muxed) {
      RTC_LOG(LS_INFO) << "Connection already exists for peer reflexive "
                          "candidate: "
                       << remote_candidate.ToSensitiveString();
      return;
    } else {
      RTC_DCHECK_NOTREACHED();
      port->SendBindingErrorResponse(stun_msg, address, STUN_ERROR_SERVER_ERROR,
                                     STUN_ERROR_REASON_SERVER_ERROR);
      return;
    }
  }

  Connection* connection =
      port->CreateConnection(remote_candidate, PortInterface::ORIGIN_THIS_PORT);
  if (!connection) {
    // This could happen in some scenarios. For example, a TurnPort may have
    // had a refresh request timeout, so it won't create connections.
    port->SendBindingErrorResponse(stun_msg, address, STUN_ERROR_SERVER_ERROR,
                                   STUN_ERROR_REASON_SERVER_ERROR);
    return;
  }

  RTC_LOG(LS_INFO) << "Adding connection from "
                   << (remote_candidate_is_new ? "peer reflexive"
                                               : "resurrected")
                   << " candidate: " << remote_candidate.ToSensitiveString();
  AddConnection(connection);
  connection->HandleStunBindingOrGoogPingRequest(stun_msg);

  // Update the list of connections since we just added another.  We do this
  // after sending the response since it could (in principle) delete the
  // connection in question.
  ice_controller_->OnImmediateSortAndSwitchRequest(
      IceSwitchReason::NEW_CONNECTION_FROM_UNKNOWN_REMOTE_ADDRESS);
}

void P2PTransportChannel::OnCandidateFilterChanged(uint32_t prev_filter,
                                                   uint32_t cur_filter) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (prev_filter == cur_filter || allocator_session() == nullptr) {
    return;
  }
  if (config_.surface_ice_candidates_on_ice_transport_type_changed) {
    allocator_session()->SetCandidateFilter(cur_filter);
  }
}

void P2PTransportChannel::OnRoleConflict(PortInterface* /* port */) {
  SignalRoleConflict(this);  // STUN ping will be sent when SetRole is called
                             // from Transport.
}

const IceParameters* P2PTransportChannel::FindRemoteIceFromUfrag(
    absl::string_view ufrag,
    uint32_t* generation) const {
  RTC_DCHECK_RUN_ON(network_thread_);
  const auto& params = remote_ice_parameters_;
  auto it = std::find_if(
      params.rbegin(), params.rend(),
      [ufrag](const IceParameters& param) { return param.ufrag == ufrag; });
  if (it == params.rend()) {
    // Not found.
    return nullptr;
  }
  *generation = params.rend() - it - 1;
  return &(*it);
}

void P2PTransportChannel::OnNominated(Connection* conn) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_DCHECK(ice_role_ == ICEROLE_CONTROLLED);

  if (selected_connection_ == conn) {
    return;
  }

  if (ice_field_trials_.send_ping_on_nomination_ice_controlled &&
      conn != nullptr) {
    SendPingRequestInternal(conn);
  }

  // TODO(qingsi): RequestSortAndStateUpdate will eventually call
  // MaybeSwitchSelectedConnection again. Rewrite this logic.
  if (ice_controller_->OnImmediateSwitchRequest(
          IceSwitchReason::NOMINATION_ON_CONTROLLED_SIDE, conn)) {
    // Now that we have selected a connection, it is time to prune other
    // connections and update the read/write state of the channel.
    ice_controller_->OnSortAndSwitchRequest(
        IceSwitchReason::NOMINATION_ON_CONTROLLED_SIDE);
  } else {
    RTC_LOG(LS_INFO)
        << "Not switching the selected connection on controlled side yet: "
        << conn->ToString();
  }
}

void P2PTransportChannel::ResolveHostnameCandidate(const Candidate& candidate) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (!async_dns_resolver_factory_) {
    RTC_LOG(LS_WARNING) << "Dropping ICE candidate with hostname address "
                           "(no AsyncResolverFactory)";
    return;
  }

  auto resolver = async_dns_resolver_factory_->Create();
  auto resptr = resolver.get();
  resolvers_.emplace_back(candidate, std::move(resolver));
  resptr->Start(candidate.address(),
                [this, resptr]() { OnCandidateResolved(resptr); });
  RTC_LOG(LS_INFO) << "Asynchronously resolving ICE candidate hostname "
                   << candidate.address().HostAsSensitiveURIString();
}

void P2PTransportChannel::AddRemoteCandidate(const Candidate& candidate) {
  RTC_DCHECK_RUN_ON(network_thread_);

  uint32_t generation = GetRemoteCandidateGeneration(candidate);
  // If a remote candidate with a previous generation arrives, drop it.
  if (generation < remote_ice_generation()) {
    RTC_LOG(LS_WARNING) << "Dropping a remote candidate because its ufrag "
                        << candidate.username()
                        << " indicates it was for a previous generation.";
    return;
  }

  Candidate new_remote_candidate(candidate);
  new_remote_candidate.set_generation(generation);
  // ICE candidates don't need to have username and password set, but
  // the code below this (specifically, ConnectionRequest::Prepare in
  // port.cc) uses the remote candidates's username.  So, we set it
  // here.
  if (remote_ice_parameters()) {
    if (candidate.username().empty()) {
      new_remote_candidate.set_username(remote_ice_parameters()->ufrag);
    }
    if (new_remote_candidate.username() == remote_ice_parameters()->ufrag) {
      if (candidate.password().empty()) {
        new_remote_candidate.set_password(remote_ice_parameters()->pwd);
      }
    } else {
      // The candidate belongs to the next generation. Its pwd will be set
      // when the new remote ICE credentials arrive.
      RTC_LOG(LS_WARNING)
          << "A remote candidate arrives with an unknown ufrag: "
          << candidate.username();
    }
  }

  if (new_remote_candidate.address().IsUnresolvedIP()) {
    // Don't do DNS lookups if the IceTransportPolicy is "none" or "relay".
    bool sharing_host = ((allocator_->candidate_filter() & CF_HOST) != 0);
    bool sharing_stun = ((allocator_->candidate_filter() & CF_REFLEXIVE) != 0);
    if (sharing_host || sharing_stun) {
      ResolveHostnameCandidate(new_remote_candidate);
    }
    return;
  }

  CheckLocalNetworkAccessPermission(new_remote_candidate);
}

P2PTransportChannel::CandidateAndResolver::CandidateAndResolver(
    const Candidate& candidate,
    std::unique_ptr<AsyncDnsResolverInterface>&& resolver)
    : candidate(candidate), resolver(std::move(resolver)) {}

P2PTransportChannel::CandidateAndResolver::~CandidateAndResolver() {}

void P2PTransportChannel::OnCandidateResolved(
    AsyncDnsResolverInterface* resolver) {
  RTC_DCHECK_RUN_ON(network_thread_);
  auto p =
      absl::c_find_if(resolvers_, [resolver](const CandidateAndResolver& cr) {
        return cr.resolver.get() == resolver;
      });
  if (p == resolvers_.end()) {
    RTC_LOG(LS_ERROR) << "Unexpected AsyncDnsResolver return";
    RTC_DCHECK_NOTREACHED();
    return;
  }
  Candidate candidate = p->candidate;
  AddRemoteCandidateWithResult(candidate, resolver->result());
  // Now we can delete the resolver.
  // TODO(bugs.webrtc.org/12651): Replace the stuff below with
  // resolvers_.erase(p);
  std::unique_ptr<AsyncDnsResolverInterface> to_delete = std::move(p->resolver);
  // Delay the actual deletion of the resolver until the lambda executes.
  network_thread_->PostTask([to_delete = std::move(to_delete)] {});
  resolvers_.erase(p);
}

void P2PTransportChannel::AddRemoteCandidateWithResult(
    Candidate candidate,
    const AsyncDnsResolverResult& result) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (result.GetError()) {
    RTC_LOG(LS_WARNING) << "Failed to resolve ICE candidate hostname "
                        << candidate.address().HostAsSensitiveURIString()
                        << " with error " << result.GetError();
    return;
  }

  SocketAddress resolved_address;
  // Prefer IPv6 to IPv4 if we have it (see RFC 5245 Section 15.1).
  // TODO(zstein): This won't work if we only have IPv4 locally but receive an
  // AAAA DNS record.
  bool have_address = result.GetResolvedAddress(AF_INET6, &resolved_address) ||
                      result.GetResolvedAddress(AF_INET, &resolved_address);
  if (!have_address) {
    RTC_LOG(LS_INFO) << "ICE candidate hostname "
                     << candidate.address().HostAsSensitiveURIString()
                     << " could not be resolved";
    return;
  }

  RTC_LOG(LS_INFO) << "Resolved ICE candidate hostname "
                   << candidate.address().HostAsSensitiveURIString() << " to "
                   << resolved_address.ipaddr().ToSensitiveString();
  candidate.set_address(resolved_address);

  CheckLocalNetworkAccessPermission(candidate);
}

void P2PTransportChannel::CheckLocalNetworkAccessPermission(
    const Candidate& candidate) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (!lna_permission_factory_) {
    RTC_LOG(LS_VERBOSE) << "No LocalNetworkAccessPermissionFactory";
    FinishAddingRemoteCandidate(candidate);
    return;
  }

  if (!candidate.address().IsPrivateIP() &&
      !candidate.address().IsLoopbackIP()) {
    RTC_LOG(LS_VERBOSE) << "Skipping LNA permission check for public IP "
                        << candidate.address().ipaddr().ToSensitiveString()
                        << ".";
    FinishAddingRemoteCandidate(candidate);
    return;
  }

  std::unique_ptr<LocalNetworkAccessPermissionInterface> permission_query =
      lna_permission_factory_->Create();
  auto permission_query_ptr = permission_query.get();
  permission_queries_.emplace_back(candidate, std::move(permission_query));

  RTC_LOG(LS_VERBOSE) << "Asynchronously requesting LNA permission."
                      << candidate.address().HostAsSensitiveURIString();
  permission_query_ptr->RequestPermission(
      candidate.address(),
      [this, permission_query_ptr](LocalNetworkAccessPermissionStatus status) {
        OnLocalNetworkAccessResult(permission_query_ptr, status);
      });
}

void P2PTransportChannel::OnLocalNetworkAccessResult(
    LocalNetworkAccessPermissionInterface* permission_query,
    LocalNetworkAccessPermissionStatus status) {
  RTC_DCHECK_RUN_ON(network_thread_);
  auto p =
      absl::c_find_if(permission_queries_,
                      [permission_query](const CandidateAndPermission& cr) {
                        return cr.permission_query.get() == permission_query;
                      });

  if (p == permission_queries_.end()) {
    RTC_LOG(LS_ERROR) << "Unexpected LocalNetworkAccessPermission return";
    RTC_DCHECK_NOTREACHED();
    return;
  }

  Candidate candidate = std::move(p->candidate);
  permission_queries_.erase(p);

  if (status != LocalNetworkAccessPermissionStatus::kGranted) {
    RTC_LOG(LS_INFO) << "LNA Permission denied for "
                     << candidate.address().HostAsSensitiveURIString() << ".";
    return;
  }

  FinishAddingRemoteCandidate(std::move(candidate));
}

void P2PTransportChannel::FinishAddingRemoteCandidate(
    const Candidate& new_remote_candidate) {
  RTC_DCHECK_RUN_ON(network_thread_);

  RTC_HISTOGRAM_ENUMERATION(
      "WebRTC.PeerConnection.CandidateAddressType",
      static_cast<int>(new_remote_candidate.address().GetIPAddressType()),
      static_cast<int>(IPAddressType::kMaxValue));

  // If this candidate matches what was thought to be a peer reflexive
  // candidate, we need to update the candidate priority/etc.
  for (Connection* conn : connections_) {
    conn->MaybeUpdatePeerReflexiveCandidate(new_remote_candidate);
  }

  // Create connections to this remote candidate.
  CreateConnections(new_remote_candidate, nullptr);

  // Resort the connections list, which may have new elements.
  ice_controller_->OnImmediateSortAndSwitchRequest(
      IceSwitchReason::NEW_CONNECTION_FROM_REMOTE_CANDIDATE);
}

void P2PTransportChannel::RemoveRemoteCandidate(
    const Candidate& cand_to_remove) {
  RTC_DCHECK_RUN_ON(network_thread_);
  auto iter =
      std::remove_if(remote_candidates_.begin(), remote_candidates_.end(),
                     [cand_to_remove](const Candidate& candidate) {
                       return cand_to_remove.MatchesForRemoval(candidate);
                     });
  if (iter != remote_candidates_.end()) {
    RTC_LOG(LS_VERBOSE) << "Removed remote candidate "
                        << cand_to_remove.ToSensitiveString();
    remote_candidates_.erase(iter, remote_candidates_.end());
  }
}

void P2PTransportChannel::RemoveAllRemoteCandidates() {
  RTC_DCHECK_RUN_ON(network_thread_);
  remote_candidates_.clear();
}

// Creates connections from all of the ports that we care about to the given
// remote candidate.  The return value is true if we created a connection from
// the origin port.
bool P2PTransportChannel::CreateConnections(const Candidate& remote_candidate,
                                            PortInterface* origin_port) {
  RTC_DCHECK_RUN_ON(network_thread_);

  // If we've already seen the new remote candidate (in the current candidate
  // generation), then we shouldn't try creating connections for it.
  // We either already have a connection for it, or we previously created one
  // and then later pruned it. If we don't return, the channel will again
  // re-create any connections that were previously pruned, which will then
  // immediately be re-pruned, churning the network for no purpose.
  // This only applies to candidates received over signaling (i.e. origin_port
  // is NULL).
  if (!origin_port && IsDuplicateRemoteCandidate(remote_candidate)) {
    // return true to indicate success, without creating any new connections.
    return true;
  }

  // Add a new connection for this candidate to every port that allows such a
  // connection (i.e., if they have compatible protocols) and that does not
  // already have a connection to an equivalent candidate.  We must be careful
  // to make sure that the origin port is included, even if it was pruned,
  // since that may be the only port that can create this connection.
  bool created = false;
  std::vector<PortInterface*>::reverse_iterator it;
  for (it = ports_.rbegin(); it != ports_.rend(); ++it) {
    if (CreateConnection(*it, remote_candidate, origin_port)) {
      if (*it == origin_port)
        created = true;
    }
  }

  if ((origin_port != nullptr) && !absl::c_linear_search(ports_, origin_port)) {
    if (CreateConnection(origin_port, remote_candidate, origin_port))
      created = true;
  }

  // Remember this remote candidate so that we can add it to future ports.
  RememberRemoteCandidate(remote_candidate, origin_port);

  return created;
}

// Setup a connection object for the local and remote candidate combination.
// And then listen to connection object for changes.
bool P2PTransportChannel::CreateConnection(PortInterface* port,
                                           const Candidate& remote_candidate,
                                           PortInterface* origin_port) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (!port->SupportsProtocol(remote_candidate.protocol())) {
    return false;
  }

  if (ice_field_trials_.skip_relay_to_non_relay_connections) {
    IceCandidateType port_type = port->Type();
    if ((port_type != remote_candidate.type()) &&
        (port_type == IceCandidateType::kRelay ||
         remote_candidate.is_relay())) {
      RTC_LOG(LS_INFO) << ToString() << ": skip creating connection "
                       << IceCandidateTypeToString(port_type) << " to "
                       << remote_candidate.type_name();
      return false;
    }
  }

  // Look for an existing connection with this remote address.  If one is not
  // found or it is found but the existing remote candidate has an older
  // generation, then we can create a new connection for this address.
  Connection* connection = port->GetConnection(remote_candidate.address());
  if (connection == nullptr || connection->remote_candidate().generation() <
                                   remote_candidate.generation()) {
    // Don't create a connection if this is a candidate we received in a
    // message and we are not allowed to make outgoing connections.
    PortInterface::CandidateOrigin origin = GetOrigin(port, origin_port);
    if (origin == PortInterface::ORIGIN_MESSAGE && incoming_only_) {
      return false;
    }
    Connection* new_connection =
        port->CreateConnection(remote_candidate, origin);
    if (!new_connection) {
      return false;
    }
    AddConnection(new_connection);
    RTC_LOG(LS_INFO) << ToString()
                     << ": Created connection with origin: " << origin
                     << ", total: " << connections_.size();
    return true;
  }

  // No new connection was created.
  // It is not legal to try to change any of the parameters of an existing
  // connection; however, the other side can send a duplicate candidate.
  if (!remote_candidate.IsEquivalent(connection->remote_candidate())) {
    RTC_LOG(LS_INFO) << "Attempt to change a remote candidate."
                        " Existing remote candidate: "
                     << connection->remote_candidate().ToSensitiveString()
                     << "New remote candidate: "
                     << remote_candidate.ToSensitiveString();
  }
  return false;
}

bool P2PTransportChannel::FindConnection(const Connection* connection) const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return absl::c_linear_search(connections_, connection);
}

uint32_t P2PTransportChannel::GetRemoteCandidateGeneration(
    const Candidate& candidate) {
  RTC_DCHECK_RUN_ON(network_thread_);
  // If the candidate has a ufrag, use it to find the generation.
  if (!candidate.username().empty()) {
    uint32_t generation = 0;
    if (!FindRemoteIceFromUfrag(candidate.username(), &generation)) {
      // If the ufrag is not found, assume the next/future generation.
      generation = static_cast<uint32_t>(remote_ice_parameters_.size());
    }
    return generation;
  }
  // If candidate generation is set, use that.
  if (candidate.generation() > 0) {
    return candidate.generation();
  }
  // Otherwise, assume the generation from remote ice parameters.
  return remote_ice_generation();
}

// Check if remote candidate is already cached.
bool P2PTransportChannel::IsDuplicateRemoteCandidate(
    const Candidate& candidate) {
  RTC_DCHECK_RUN_ON(network_thread_);
  for (size_t i = 0; i < remote_candidates_.size(); ++i) {
    if (remote_candidates_[i].IsEquivalent(candidate)) {
      return true;
    }
  }
  return false;
}

// Maintain our remote candidate list, adding this new remote one.
void P2PTransportChannel::RememberRemoteCandidate(
    const Candidate& remote_candidate,
    PortInterface* origin_port) {
  RTC_DCHECK_RUN_ON(network_thread_);
  // Remove any candidates whose generation is older than this one.  The
  // presence of a new generation indicates that the old ones are not useful.
  size_t i = 0;
  while (i < remote_candidates_.size()) {
    if (remote_candidates_[i].generation() < remote_candidate.generation()) {
      RTC_LOG(LS_INFO) << "Pruning candidate from old generation: "
                       << remote_candidates_[i].address().ToSensitiveString();
      remote_candidates_.erase(remote_candidates_.begin() + i);
    } else {
      i += 1;
    }
  }

  // Make sure this candidate is not a duplicate.
  if (IsDuplicateRemoteCandidate(remote_candidate)) {
    RTC_LOG(LS_INFO) << "Duplicate candidate: "
                     << remote_candidate.ToSensitiveString();
    return;
  }

  // Try this candidate for all future ports.
  remote_candidates_.push_back(RemoteCandidate(remote_candidate, origin_port));
}

// Set options on ourselves is simply setting options on all of our available
// port objects.
int P2PTransportChannel::SetOption(Socket::Option opt, int value) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (ice_field_trials_.override_dscp && opt == Socket::OPT_DSCP) {
    value = *ice_field_trials_.override_dscp;
  }

  OptionMap::iterator it = options_.find(opt);
  if (it == options_.end()) {
    options_.insert(std::make_pair(opt, value));
  } else if (it->second == value) {
    return 0;
  } else {
    it->second = value;
  }

  for (PortInterface* port : ports_) {
    int val = port->SetOption(opt, value);
    if (val < 0) {
      // Because this also occurs deferred, probably no point in reporting an
      // error
      RTC_LOG(LS_WARNING) << "SetOption(" << opt << ", " << value
                          << ") failed: " << port->GetError();
    }
  }
  return 0;
}

bool P2PTransportChannel::GetOption(Socket::Option opt, int* value) {
  RTC_DCHECK_RUN_ON(network_thread_);

  const auto& found = options_.find(opt);
  if (found == options_.end()) {
    return false;
  }
  *value = found->second;
  return true;
}

int P2PTransportChannel::GetError() {
  RTC_DCHECK_RUN_ON(network_thread_);
  return error_;
}
// [新增] 从 Controller 获取最新的次优路径并缓存
void P2PTransportChannel::UpdateSecondSelectedConnection() {
  RTC_DCHECK_RUN_ON(network_thread_);
  
  if (!IceMultipathDualPathSendActive()) {
    second_selected_connection_ = nullptr;
    return;
  }

  // 调用之前我们在 Controller 中打通的接口
  const Connection* sec_conn = ice_controller_->GetSecondBestConnection();
  
  if (sec_conn) {
    // 使用 FromIceController 将 const 指针转为非 const (包含安全检查)
    second_selected_connection_ = FromIceController(sec_conn);
  } else {
    second_selected_connection_ = nullptr;
  }
}
// Send data to the other side, using our selected connection.
int P2PTransportChannel::SendPacket(const char* data,
                                    size_t len,
                                    const AsyncSocketPacketOptions& options,
                                    int flags) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (flags != 0) {
    error_ = EINVAL;
    return -1;
  }

  Connection* primary = selected_connection_;
  Connection* secondary = nullptr;

  const bool is_rtp_rtcp = LooksLikeRtpOrRtcp(data, len);
  const bool allow_multipath =
      IceMultipathDualPathSendActive() &&
      (!kIceMultipathOnlyRtpRtcp || is_rtp_rtcp);

  if (allow_multipath) {
    secondary = second_selected_connection_;
  }

  bool can_use_multipath = (secondary != nullptr && ReadyToSend(secondary) &&
                            ReadyToSend(primary));

  AsyncSocketPacketOptions modified_options(options);
  modified_options.info_signaled_after_sent.packet_type = PacketType::kData;

  int final_result = -1;
  bool packet_sent = false;
  const int packet_priority = options.packet_priority;
  const bool prefer_primary =
      packet_priority == kPacketPriorityRetransmission ||
      packet_priority == kPacketPriorityAudio ||
      packet_priority == kPacketPriorityKeyFrame;
  const bool is_redundancy = packet_priority == kPacketPriorityRedundancy;
  const bool is_other = packet_priority == kPacketPriorityOther;
  const bool is_probe = options.probe_cluster_id != kNoProbeClusterId;
  const bool hinted_primary = options.path_hint == 0;
  const bool hinted_secondary = options.path_hint == 1;
  const bool hinted_video_media_packet =
      (hinted_primary || hinted_secondary) &&
      packet_priority != kPacketPriorityAudio &&
      packet_priority != kPacketPriorityRetransmission &&
      packet_priority != kPacketPriorityRedundancy;

  const auto has_budget = [len](uint64_t bytes_budget, uint32_t pkt_budget) {
    return pkt_budget > 0 && bytes_budget >= len;
  };
  const auto consume_budget = [len](uint64_t& bytes_budget,
                                    uint32_t& pkt_budget) {
    if (pkt_budget > 0) {
      --pkt_budget;
    }
    bytes_budget = (bytes_budget > len) ? (bytes_budget - len) : 0;
  };

  const int64_t now_ms = TimeMillis();
  const bool frame_interval_observable_packet =
      is_rtp_rtcp && packet_priority != kPacketPriorityAudio &&
      packet_priority != kPacketPriorityRetransmission &&
      packet_priority != kPacketPriorityRedundancy;
  const bool enable_frame_interval_resort =
      GetWebrtcMultipathSendMode() == WebrtcMultipathSendMode::kExtended &&
      can_use_multipath;
  if (enable_frame_interval_resort && frame_interval_observable_packet) {
    std::optional<RtpFrameKey> frame_key = ParseRtpFrameKey(data, len);
    if (frame_key.has_value()) {
      const bool new_frame =
          !frame_interval_last_frame_valid_ ||
          frame_key->ssrc != frame_interval_last_frame_ssrc_ ||
          frame_key->timestamp != frame_interval_last_frame_timestamp_;
      if (new_frame) {
        if (frame_interval_last_frame_valid_ &&
            frame_interval_last_frame_send_ms_ > 0) {
          const int64_t interval_ms = now_ms - frame_interval_last_frame_send_ms_;
          if (interval_ms > 0) {
            if (frame_interval_ewma_ms_ <= 0) {
              frame_interval_ewma_ms_ = interval_ms;
            } else {
              const int64_t ratio_threshold_ms =
                  frame_interval_ewma_ms_ *
                  kFrameIntervalIncreaseTriggerRatioPercent / 100;
              const int64_t min_delta_threshold_ms =
                  frame_interval_ewma_ms_ + kFrameIntervalIncreaseMinDeltaMs;
              const int64_t trigger_threshold_ms =
                  std::max(ratio_threshold_ms, min_delta_threshold_ms);
              const bool interval_increased = interval_ms >= trigger_threshold_ms;
              if (interval_increased) {
                ++frame_interval_increase_streak_;
              } else {
                frame_interval_increase_streak_ = 0;
              }
              const bool can_trigger_resort =
                  last_frame_interval_sort_request_ms_ == 0 ||
                  now_ms - last_frame_interval_sort_request_ms_ >=
                      kFrameIntervalResortMinIntervalMs;
              if (interval_increased && can_trigger_resort &&
                  frame_interval_increase_streak_ >=
                      kFrameIntervalIncreaseConsecutiveFrames) {
                last_frame_interval_sort_request_ms_ = now_ms;
                frame_interval_increase_streak_ = 0;
                ice_controller_->OnSortAndSwitchRequest(
                    IceSwitchReason::APPLICATION_REQUESTED);
              }
              frame_interval_ewma_ms_ =
                  (frame_interval_ewma_ms_ * kFrameIntervalEwmaNumerator +
                   interval_ms) /
                  kFrameIntervalEwmaDenominator;
            }
          }
        }
        frame_interval_last_frame_valid_ = true;
        frame_interval_last_frame_ssrc_ = frame_key->ssrc;
        frame_interval_last_frame_timestamp_ = frame_key->timestamp;
        frame_interval_last_frame_send_ms_ = now_ms;
      }
    }
  } else if (!enable_frame_interval_resort) {
    frame_interval_last_frame_valid_ = false;
    frame_interval_last_frame_ssrc_ = 0;
    frame_interval_last_frame_timestamp_ = 0;
    frame_interval_last_frame_send_ms_ = 0;
    frame_interval_ewma_ms_ = 0;
    frame_interval_increase_streak_ = 0;
    last_frame_interval_sort_request_ms_ = 0;
  }

  const int requested_window_ms = std::max(
      kMultipathMinScheduleWindowMs,
      std::min(kMultipathMaxScheduleWindowMs,
               options.batch_duration_ms >= 0
                   ? options.batch_duration_ms
                   : static_cast<int>(kMultipathScheduleWindowMs)));
  const auto log_window_quota_summary_if_needed = [&]() {
    if (sched_window_start_ms_ == 0) {
      return;
    }
    if (sched_window_count_ % kMultipathQuotaLogIntervalBatches == 0) {
      RTC_LOG(LS_INFO)
          << "MP-SCHED BatchQuota: window=" << sched_window_count_
          << ", batches_in_window=" << batches_in_current_window_
          << ", duration_ms=" << sched_window_duration_ms_
          << ", quota_bytes(primary/secondary)=" << primary_batch_quota_bytes_
          << "/" << secondary_batch_quota_bytes_
          << ", est_bps(primary/secondary)=" << primary_estimated_bps_ << "/"
          << secondary_estimated_bps_
          << ", est_src(primary/secondary)="
          << (primary_bitrate_from_probe_ ? "probe_feedback" : "stats")
          << "/" << (secondary_bitrate_from_probe_ ? "probe_feedback"
                                                   : "stats")
          << ", wait_probe(primary/secondary)=" << wait_probe_for_primary_path_
          << "/" << wait_probe_for_secondary_path_;
    }
  };
  const auto log_window_over_quota_if_needed = [&]() {
    if (sched_window_start_ms_ == 0) {
      return;
    }
    // Suppressed noisy over-quota logs after removing quota-based scheduling.
  };

  if (((!can_use_multipath || IsWebrtcMultipathRoundRobinMode()) &&
       !IsSinglePathPacerPriorityMode())) {
    log_window_quota_summary_if_needed();
    log_window_over_quota_if_needed();
    sched_window_start_ms_ = 0;
    sched_window_duration_ms_ = 0;
    primary_budget_bytes_remaining_ = 0;
    secondary_budget_bytes_remaining_ = 0;
    primary_budget_packets_remaining_ = 0;
    secondary_budget_packets_remaining_ = 0;
    primary_non_vip_budget_bytes_remaining_ = 0;
    primary_non_vip_budget_packets_remaining_ = 0;
    other_primary_current_weight_ = 0;
    other_secondary_current_weight_ = 0;
    primary_batch_quota_bytes_ = 0;
    secondary_batch_quota_bytes_ = 0;
    primary_batch_sent_bytes_ = 0;
    secondary_batch_sent_bytes_ = 0;
    primary_over_quota_packets_in_window_ = 0;
    secondary_over_quota_packets_in_window_ = 0;
    batches_in_current_window_ = 0;
    last_periodic_probe_request_ms_ = 0;
    frame_interval_last_frame_valid_ = false;
    frame_interval_last_frame_ssrc_ = 0;
    frame_interval_last_frame_timestamp_ = 0;
    frame_interval_last_frame_send_ms_ = 0;
    frame_interval_ewma_ms_ = 0;
    frame_interval_increase_streak_ = 0;
    last_frame_interval_sort_request_ms_ = 0;
    wait_probe_for_primary_path_ = false;
    wait_probe_for_secondary_path_ = false;
    multipath_round_robin_frame_valid_ = false;
    UpdateMultipathQuotaHint(MultipathQuotaHint());
  } else if (!IsWebrtcMultipathRoundRobinMode() &&
             (can_use_multipath || IsSinglePathPacerPriorityMode()) &&
             primary && ReadyToSend(primary) &&
             (sched_window_start_ms_ == 0 ||
              now_ms - sched_window_start_ms_ >= sched_window_duration_ms_)) {
    log_window_quota_summary_if_needed();
    log_window_over_quota_if_needed();
    sched_window_start_ms_ = now_ms;
    sched_window_duration_ms_ = requested_window_ms;
    ++sched_window_count_;
    batches_in_current_window_ = 0;
    const int batch_duration_ms = sched_window_duration_ms_;
    Connection* const secondary_for_stats =
        can_use_multipath ? secondary : primary;
    ConnectionInfo primary_stats = primary->stats();
    ConnectionInfo secondary_stats = secondary_for_stats->stats();
    const uint16_t primary_network_id = primary->local_candidate().network_id();
    const uint16_t secondary_network_id =
        secondary_for_stats->local_candidate().network_id();
    const std::optional<uint32_t> primary_probe_bps =
        GetProbeBitrateForNetwork(primary_network_id);
    const std::optional<uint32_t> secondary_probe_bps =
        GetProbeBitrateForNetwork(secondary_network_id);
    const std::optional<uint32_t> primary_delay_bps =
        GetDelayBasedBitrateForNetwork(primary_network_id);
    const std::optional<uint32_t> secondary_delay_bps =
        GetDelayBasedBitrateForNetwork(secondary_network_id);
    primary_bitrate_from_probe_ = primary_probe_bps.has_value();
    secondary_bitrate_from_probe_ = secondary_probe_bps.has_value();

    const uint32_t sampled_primary_bps =
        primary_delay_bps.has_value()
            ? *primary_delay_bps
            : (primary_probe_bps.has_value()
                   ? *primary_probe_bps
                   : (primary_stats.sent_bytes_second > 0
                          ? static_cast<uint32_t>(primary_stats.sent_bytes_second)
                          : kMultipathDefaultEstimatedBps));
    const uint32_t sampled_secondary_bps =
        secondary_delay_bps.has_value()
            ? *secondary_delay_bps
            : (secondary_probe_bps.has_value()
                   ? *secondary_probe_bps
                   : (secondary_stats.sent_bytes_second > 0
                          ? static_cast<uint32_t>(
                                secondary_stats.sent_bytes_second)
                          : kMultipathDefaultEstimatedBps));
    const size_t avg_pkt = std::max<size_t>(sched_avg_packet_size_bytes_, 200);

    if (primary_estimated_bps_ == 0) {
      primary_estimated_bps_ = sampled_primary_bps;
    } else {
      primary_estimated_bps_ =
          (primary_estimated_bps_ * 7 + sampled_primary_bps) / 8;
    }
    if (secondary_estimated_bps_ == 0) {
      secondary_estimated_bps_ = sampled_secondary_bps;
    } else {
      secondary_estimated_bps_ =
          (secondary_estimated_bps_ * 7 + sampled_secondary_bps) / 8;
    }
    if (IsSinglePathPacerPriorityMode()) {
      secondary_estimated_bps_ = primary_estimated_bps_;
    }

    const uint64_t primary_budget_from_bps =
        (static_cast<uint64_t>(primary_estimated_bps_) * batch_duration_ms) /
        8000;
    const uint64_t secondary_budget_from_bps =
        (static_cast<uint64_t>(secondary_estimated_bps_) * batch_duration_ms) /
        8000;
    const uint64_t primary_optimistic_budget =
        primary_budget_from_bps * kMultipathOptimismNumerator /
        kMultipathOptimismDenominator;
    const uint64_t secondary_optimistic_budget =
        secondary_budget_from_bps * kMultipathOptimismNumerator /
        kMultipathOptimismDenominator;
    const uint64_t primary_compensated_budget =
        primary_optimistic_budget + static_cast<uint64_t>(avg_pkt);
    const uint64_t secondary_compensated_budget =
        secondary_optimistic_budget + static_cast<uint64_t>(avg_pkt);
    primary_budget_bytes_remaining_ =
        std::max<uint64_t>(kMultipathMinBudgetBytes, primary_compensated_budget);
    secondary_budget_bytes_remaining_ =
        std::max<uint64_t>(kMultipathMinBudgetBytes,
                           secondary_compensated_budget);
    primary_batch_quota_bytes_ = primary_budget_bytes_remaining_;
    secondary_batch_quota_bytes_ = secondary_budget_bytes_remaining_;
    primary_batch_sent_bytes_ = 0;
    secondary_batch_sent_bytes_ = 0;
    primary_over_quota_packets_in_window_ = 0;
    secondary_over_quota_packets_in_window_ = 0;

    primary_budget_packets_remaining_ = std::max<uint32_t>(
        kMultipathMinBudgetPackets, primary_budget_bytes_remaining_ / avg_pkt);
    secondary_budget_packets_remaining_ = std::max<uint32_t>(
        kMultipathMinBudgetPackets, secondary_budget_bytes_remaining_ / avg_pkt);

    // Reserve part of primary budget for high-priority packets in each window.
    primary_non_vip_budget_bytes_remaining_ = primary_budget_bytes_remaining_ / 3;
    primary_non_vip_budget_packets_remaining_ =
        std::max<uint32_t>(1, primary_budget_packets_remaining_ / 3);
    other_primary_current_weight_ = 0;
    other_secondary_current_weight_ = 0;

    if (primary_network_id != last_primary_network_id_for_probe_) {
      last_primary_network_id_for_probe_ = primary_network_id;
      wait_probe_for_primary_path_ = !primary_probe_bps.has_value();
      RequestGccProbeOnNewPath();
    } else {
      wait_probe_for_primary_path_ = !primary_probe_bps.has_value();
    }
    if (secondary_network_id != last_secondary_network_id_for_probe_) {
      last_secondary_network_id_for_probe_ = secondary_network_id;
      wait_probe_for_secondary_path_ = !secondary_probe_bps.has_value();
      RequestGccProbeOnNewPath();
    } else {
      wait_probe_for_secondary_path_ = !secondary_probe_bps.has_value();
    }
    const bool should_request_periodic_probe =
        last_periodic_probe_request_ms_ == 0 ||
        now_ms - last_periodic_probe_request_ms_ >= kPeriodicProbeIntervalMs;
    if (should_request_periodic_probe) {
      RequestGccProbeOnNewPath();
      last_periodic_probe_request_ms_ = now_ms;
    }
    MultipathQuotaHint multipath_hint;
    multipath_hint.primary_estimated_bps = primary_estimated_bps_;
    multipath_hint.secondary_estimated_bps = secondary_estimated_bps_;
    multipath_hint.schedule_window_ms = sched_window_duration_ms_;
    UpdateMultipathQuotaHint(multipath_hint);
    if (options.first_packet_in_batch) {
      ++batches_in_current_window_;
    }
  } else if (options.first_packet_in_batch) {
    ++batches_in_current_window_;
  }

  if (!can_use_multipath) {
    if (primary && ReadyToSend(primary)) {
      final_result = primary->Send(data, len, modified_options);
      packet_sent = final_result > 0;
      if (packet_sent && is_probe) {
        ++primary_probe_packets_sent_;
      }
    }
  } else if (IsWebrtcMultipathRoundRobinMode()) {
    Connection* preferred = nullptr;
    Connection* alternate = nullptr;
    if (is_probe) {
      // Keep probing orthogonal to media round-robin so probe packets don't
      // disturb frame-level path alternation.
      const bool probe_to_secondary =
          (static_cast<uint32_t>(options.probe_cluster_id) & 0x1u) != 0;
      preferred = probe_to_secondary ? secondary : primary;
      alternate = probe_to_secondary ? primary : secondary;
    } else {
      bool use_frame_level_round_robin =
          packet_priority != kPacketPriorityAudio &&
          packet_priority != kPacketPriorityRetransmission &&
          packet_priority != kPacketPriorityRedundancy;
      if (use_frame_level_round_robin) {
        std::optional<RtpFrameKey> frame_key = ParseRtpFrameKey(data, len);
        if (frame_key.has_value()) {
          if (!multipath_round_robin_frame_valid_) {
            multipath_round_robin_frame_valid_ = true;
            multipath_round_robin_frame_ssrc_ = frame_key->ssrc;
            multipath_round_robin_frame_timestamp_ = frame_key->timestamp;
          } else if (multipath_round_robin_frame_ssrc_ != frame_key->ssrc ||
                     multipath_round_robin_frame_timestamp_ !=
                         frame_key->timestamp) {
            ++multipath_round_robin_phase_;
            multipath_round_robin_frame_ssrc_ = frame_key->ssrc;
            multipath_round_robin_frame_timestamp_ = frame_key->timestamp;
          }
        } else {
          use_frame_level_round_robin = false;
        }
      }
      if (!use_frame_level_round_robin) {
        if (options.last_packet_in_batch) {
          ++multipath_round_robin_phase_;
        }
      }
      preferred = (multipath_round_robin_phase_ & 1u) ? secondary : primary;
      alternate = (multipath_round_robin_phase_ & 1u) ? primary : secondary;
    }
    Connection* sent_on = nullptr;
    final_result = -1;
    if (preferred && ReadyToSend(preferred)) {
      final_result = preferred->Send(data, len, modified_options);
      if (final_result > 0) {
        sent_on = preferred;
      }
    }
    if (final_result <= 0 && alternate && ReadyToSend(alternate)) {
      final_result = alternate->Send(data, len, modified_options);
      if (final_result > 0) {
        sent_on = alternate;
      }
    }
    packet_sent = final_result > 0;
    if (packet_sent) {
      if (is_probe && sent_on) {
        if (sent_on == primary) {
          ++primary_probe_packets_sent_;
        } else {
          ++secondary_probe_packets_sent_;
        }
      }
    }
  } else {
    Connection* target = primary;
    Connection* backup = secondary;

    if (is_probe && wait_probe_for_secondary_path_) {
      target = secondary;
      backup = primary;
    } else if (is_probe && wait_probe_for_primary_path_) {
      target = primary;
      backup = secondary;
    } else if (is_probe) {
      // Route GCC probe clusters deterministically per path so both paths get
      // active probe bursts and their capacities can be explored.
      const bool probe_to_secondary =
          (static_cast<uint32_t>(options.probe_cluster_id) & 0x1u) != 0;
      target = probe_to_secondary ? secondary : primary;
      backup = probe_to_secondary ? primary : secondary;
    } else if (hinted_primary || hinted_secondary) {
      target = hinted_secondary ? secondary : primary;
      backup = hinted_secondary ? primary : secondary;
      if (!hinted_video_media_packet) {
        if (target == primary &&
            !has_budget(primary_budget_bytes_remaining_,
                        primary_budget_packets_remaining_) &&
            has_budget(secondary_budget_bytes_remaining_,
                       secondary_budget_packets_remaining_)) {
          target = secondary;
          backup = primary;
        } else if (target == secondary &&
                   !has_budget(secondary_budget_bytes_remaining_,
                               secondary_budget_packets_remaining_) &&
                   has_budget(primary_budget_bytes_remaining_,
                              primary_budget_packets_remaining_)) {
          target = primary;
          backup = secondary;
        }
      }
    } else if (prefer_primary) {
      // Retransmission/audio/keyframe are preferentially mapped to primary.
      if (!has_budget(primary_budget_bytes_remaining_,
                      primary_budget_packets_remaining_) &&
          has_budget(secondary_budget_bytes_remaining_,
                     secondary_budget_packets_remaining_)) {
        target = secondary;
        backup = primary;
      }
    } else {
      if (is_other) {
        // For "other" traffic, distribute by remaining-budget ratio using a
        // smooth weighted round robin, so both paths keep sending over time.
        const uint64_t total_remaining =
            primary_budget_bytes_remaining_ + secondary_budget_bytes_remaining_;
        if (total_remaining > 0) {
          other_primary_current_weight_ +=
              static_cast<int64_t>(primary_budget_bytes_remaining_);
          other_secondary_current_weight_ +=
              static_cast<int64_t>(secondary_budget_bytes_remaining_);
          const bool should_send_primary =
              other_primary_current_weight_ >= other_secondary_current_weight_;
          target = should_send_primary ? primary : secondary;
          backup = should_send_primary ? secondary : primary;
          if (should_send_primary) {
            other_primary_current_weight_ -= static_cast<int64_t>(total_remaining);
          } else {
            other_secondary_current_weight_ -=
                static_cast<int64_t>(total_remaining);
          }
        } else {
          target = secondary;
          backup = primary;
        }
      } else if (is_redundancy) {
        // Redundancy packets are pushed to secondary first to leave primary
        // headroom for retransmission/audio/keyframe.
        if (has_budget(secondary_budget_bytes_remaining_,
                       secondary_budget_packets_remaining_)) {
          target = secondary;
          backup = primary;
        } else if (!has_budget(primary_non_vip_budget_bytes_remaining_,
                               primary_non_vip_budget_packets_remaining_)) {
          if (primary_budget_bytes_remaining_ <
              secondary_budget_bytes_remaining_) {
            target = secondary;
            backup = primary;
          }
        }
      } else {
        target = primary;
        backup = secondary;
      }
    }

    if (!is_probe && !hinted_video_media_packet &&
        wait_probe_for_secondary_path_ && target == secondary) {
      target = primary;
      backup = secondary;
    }
    if (!is_probe && !hinted_video_media_packet &&
        wait_probe_for_primary_path_ && target == primary &&
        !wait_probe_for_secondary_path_) {
      target = secondary;
      backup = primary;
    }

    final_result = target->Send(data, len, modified_options);
    packet_sent = final_result > 0;
    if (!packet_sent) {
      final_result = backup->Send(data, len, modified_options);
      packet_sent = final_result > 0;
      if (packet_sent) {
        target = backup;
      }
    }

    if (packet_sent) {
      if (target == primary) {
        primary_batch_sent_bytes_ += static_cast<uint64_t>(final_result);
      } else {
        secondary_batch_sent_bytes_ += static_cast<uint64_t>(final_result);
      }
      if (target == primary) {
        consume_budget(primary_budget_bytes_remaining_,
                       primary_budget_packets_remaining_);
        if (!prefer_primary) {
          consume_budget(primary_non_vip_budget_bytes_remaining_,
                         primary_non_vip_budget_packets_remaining_);
        }
      } else {
        consume_budget(secondary_budget_bytes_remaining_,
                       secondary_budget_packets_remaining_);
      }
      if (is_probe) {
        if (target == primary) {
          ++primary_probe_packets_sent_;
        } else {
          ++secondary_probe_packets_sent_;
        }
      }
      if (target == primary && primary_batch_quota_bytes_ > 0 &&
          primary_batch_sent_bytes_ > primary_batch_quota_bytes_) {
        ++primary_over_quota_packets_in_window_;
      }
      if (target == secondary && secondary_batch_quota_bytes_ > 0 &&
          secondary_batch_sent_bytes_ > secondary_batch_quota_bytes_) {
        ++secondary_over_quota_packets_in_window_;
      }
    }
  }

  if (!packet_sent) {
    error_ = ENOTCONN;
    return -1;
  }

  if (final_result > 0) {
    packets_sent_++;
    bytes_sent_ += final_result;
    last_sent_packet_id_ = options.packet_id;
    sched_avg_packet_size_bytes_ =
        (sched_avg_packet_size_bytes_ * 7 + static_cast<size_t>(final_result)) /
        8;

    const int clamped_priority =
        std::max(kPacketPriorityRetransmission,
                 std::min(kPacketPriorityOther, packet_priority));
    priority_packets_in_log_window_[clamped_priority]++;

    if (packets_sent_ > 0 &&
        packets_sent_ % kMultipathSchedulerLogIntervalPackets == 0) {
      RTC_LOG(LS_INFO)
          << "MP-SCHED Stats: total=" << packets_sent_
          << ", prio@1k[r/a/k/rd/o]="
          << priority_packets_in_log_window_[kPacketPriorityRetransmission]
          << "/" << priority_packets_in_log_window_[kPacketPriorityAudio] << "/"
          << priority_packets_in_log_window_[kPacketPriorityKeyFrame] << "/"
          << priority_packets_in_log_window_[kPacketPriorityRedundancy] << "/"
          << priority_packets_in_log_window_[kPacketPriorityOther]
          << ", probes(primary/secondary)=" << primary_probe_packets_sent_
          << "/" << secondary_probe_packets_sent_
          << ", est_bps(primary/secondary)=" << primary_estimated_bps_ << "/"
          << secondary_estimated_bps_
          << ", est_src(primary/secondary)="
          << (primary_bitrate_from_probe_ ? "probe_feedback" : "stats")
          << "/" << (secondary_bitrate_from_probe_ ? "probe_feedback"
                                                   : "stats");
      priority_packets_in_log_window_.fill(0);
    }
  }

  return final_result;
}

bool P2PTransportChannel::GetStats(IceTransportStats* ice_transport_stats) {
  RTC_DCHECK_RUN_ON(network_thread_);
  // Gather candidate and candidate pair stats.
  ice_transport_stats->candidate_stats_list.clear();
  ice_transport_stats->connection_infos.clear();

  if (!allocator_sessions_.empty()) {
    allocator_session()->GetCandidateStatsFromReadyPorts(
        &ice_transport_stats->candidate_stats_list);
  }

  // TODO(qingsi): Remove naming inconsistency for candidate pair/connection.
  for (Connection* connection : connections_) {
    ConnectionInfo stats = connection->stats();
    stats.local_candidate = SanitizeLocalCandidate(stats.local_candidate);
    stats.remote_candidate = SanitizeRemoteCandidate(stats.remote_candidate);
    stats.best_connection = (selected_connection_ == connection);
    ice_transport_stats->connection_infos.push_back(std::move(stats));
  }

  ice_transport_stats->selected_candidate_pair_changes =
      selected_candidate_pair_changes_;

  ice_transport_stats->bytes_sent = bytes_sent_;
  ice_transport_stats->bytes_received = bytes_received_;
  ice_transport_stats->packets_sent = packets_sent_;
  ice_transport_stats->packets_received = packets_received_;

  ice_transport_stats->ice_role = GetIceRole();
  ice_transport_stats->ice_local_username_fragment = ice_parameters_.ufrag;
  ice_transport_stats->ice_state = ComputeIceTransportState();

  return true;
}

std::optional<NetworkRoute> P2PTransportChannel::network_route() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return network_route_;
}

DiffServCodePoint P2PTransportChannel::DefaultDscpValue() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  OptionMap::const_iterator it = options_.find(Socket::OPT_DSCP);
  if (it == options_.end()) {
    return DSCP_NO_CHANGE;
  }
  return static_cast<DiffServCodePoint>(it->second);
}

ArrayView<Connection* const> P2PTransportChannel::connections() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return ArrayView<Connection* const>(connections_.data(), connections_.size());
}

void P2PTransportChannel::RemoveConnectionForTest(Connection* connection) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_DCHECK(FindConnection(connection));
  connection->SignalDestroyed.disconnect(this);
  RemoveConnection(connection);
  RTC_DCHECK(!FindConnection(connection));
  if (selected_connection_ == connection)
    selected_connection_ = nullptr;
  connection->Destroy();
}

// Monitor connection states.
void P2PTransportChannel::UpdateConnectionStates() {
  RTC_DCHECK_RUN_ON(network_thread_);
  int64_t now = TimeMillis();

  // We need to copy the list of connections since some may delete themselves
  // when we call UpdateState.
  // NOTE: We copy the connections() vector in case `UpdateState` triggers the
  // Connection to be destroyed (which will cause a callback that alters
  // the connections() vector).
  std::vector<Connection*> copy(connections_.begin(), connections_.end());
  for (Connection* c : copy) {
    c->UpdateState(now);
  }
}

void P2PTransportChannel::OnStartedPinging() {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_LOG(LS_INFO) << ToString()
                   << ": Have a pingable connection for the first time; "
                      "starting to ping.";
  regathering_controller_->Start();
}

bool P2PTransportChannel::IsPortPruned(const PortInterface* port) const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return !absl::c_linear_search(ports_, port);
}

bool P2PTransportChannel::IsRemoteCandidatePruned(const Candidate& cand) const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return !absl::c_linear_search(remote_candidates_, cand);
}

bool P2PTransportChannel::PresumedWritable(const Connection* conn) const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return (conn->write_state() == Connection::STATE_WRITE_INIT &&
          config_.presume_writable_when_fully_relayed &&
          conn->local_candidate().is_relay() &&
          (conn->remote_candidate().is_relay() ||
           conn->remote_candidate().is_prflx()));
}

void P2PTransportChannel::UpdateState() {
  RTC_DCHECK_RUN_ON(network_thread_);

  // Check if all connections are timedout.
  bool all_connections_timedout = true;
  for (const Connection* conn : connections_) {
    if (conn->write_state() != Connection::STATE_WRITE_TIMEOUT) {
      all_connections_timedout = false;
      break;
    }
  }

  // Now update the writable state of the channel with the information we have
  // so far.
  if (all_connections_timedout) {
    HandleAllTimedOut();
  }

  // Update the state of this channel.
  UpdateTransportState();
}

bool P2PTransportChannel::AllowedToPruneConnections() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return ice_role_ == ICEROLE_CONTROLLING ||
         (selected_connection_ && selected_connection_->nominated());
}

bool P2PTransportChannel::PruneConnections(
    ArrayView<const Connection* const> connections) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (!AllowedToPruneConnections()) {
    RTC_LOG(LS_WARNING) << "Not allowed to prune connections";
    return false;
  }

  // Minimal multipath: keep one extra (non-selected) connection alive if possible.
  const Connection* keep_conn = nullptr;

  if (IceMultipathDualPathSendActive() && selected_connection_ != nullptr &&
      connections.size() >= 2) {
    const uint16_t selected_network_id =
        selected_connection_->local_candidate().network_id();
    const bool prefer_bandwidth_before_rtt =
        GetWebrtcMultipathSendMode() == WebrtcMultipathSendMode::kExtended;
    const auto get_estimated_bps = [](const Connection* conn) -> uint32_t {
      const uint16_t network_id = conn->local_candidate().network_id();
      const std::optional<uint32_t> delay_bps =
          GetDelayBasedBitrateForNetwork(network_id);
      if (delay_bps.has_value()) {
        return *delay_bps;
      }
      const std::optional<uint32_t> probe_bps =
          GetProbeBitrateForNetwork(network_id);
      if (probe_bps.has_value()) {
        return *probe_bps;
      }
      return 0u;
    };

    const Connection* best = nullptr;
    int best_rtt = (1 << 30);
    uint32_t best_estimated_bps = 0;

    // Pass 1: Prefer different local network_id (if enabled).
    if (kIceMultipathPreferDifferentNetwork) {
      for (const Connection* c : connections) {
        if (c == nullptr || c == selected_connection_) {
          continue;
        }
        if (c->write_state() == Connection::STATE_WRITE_TIMEOUT) {
          continue;
        }
        if (c->local_candidate().network_id() == selected_network_id) {
          continue;
        }
        const uint32_t estimated_bps = get_estimated_bps(c);
        const int rtt = (c->rtt_samples() > 0) ? c->rtt() : (1 << 29);
        const bool is_better =
            best == nullptr ||
            (prefer_bandwidth_before_rtt &&
             (estimated_bps > best_estimated_bps ||
              (estimated_bps == best_estimated_bps && rtt < best_rtt))) ||
            (!prefer_bandwidth_before_rtt && rtt < best_rtt);
        if (is_better) {
          best = c;
          best_rtt = rtt;
          best_estimated_bps = estimated_bps;
        }
      }
    }

    // Pass 2: Fallback to any other candidate in the prune set.
    if (best == nullptr) {
      best_rtt = (1 << 30);
      best_estimated_bps = 0;
      for (const Connection* c : connections) {
        if (c == nullptr || c == selected_connection_) {
          continue;
        }
        if (c->write_state() == Connection::STATE_WRITE_TIMEOUT) {
          continue;
        }
        const uint32_t estimated_bps = get_estimated_bps(c);
        const int rtt = (c->rtt_samples() > 0) ? c->rtt() : (1 << 29);
        const bool is_better =
            best == nullptr ||
            (prefer_bandwidth_before_rtt &&
             (estimated_bps > best_estimated_bps ||
              (estimated_bps == best_estimated_bps && rtt < best_rtt))) ||
            (!prefer_bandwidth_before_rtt && rtt < best_rtt);
        if (is_better) {
          best = c;
          best_rtt = rtt;
          best_estimated_bps = estimated_bps;
        }
      }
    }

    keep_conn = best;
  }

  for (const Connection* conn : connections) {
    if (conn == selected_connection_ || conn == keep_conn) {
      continue;
    }
    FromIceController(conn)->Prune();
  }
  return true;
}


NetworkRoute P2PTransportChannel::ConfigureNetworkRoute(
    const Connection* conn) {
  RTC_DCHECK_RUN_ON(network_thread_);
  return {.connected = ReadyToSend(conn),
          .local = CreateRouteEndpointFromCandidate(
              /* local= */ true, conn->local_candidate(),
              /* uses_turn= */
              conn->port()->Type() == IceCandidateType::kRelay),
          .remote = CreateRouteEndpointFromCandidate(
              /* local= */ false, conn->remote_candidate(),
              /* uses_turn= */ conn->remote_candidate().is_relay()),
          .last_sent_packet_id = last_sent_packet_id_,
          .packet_overhead =
              conn->local_candidate().address().ipaddr().overhead() +
              GetProtocolOverhead(conn->local_candidate().protocol())};
}

void P2PTransportChannel::SwitchSelectedConnection(
    const Connection* new_connection,
    IceSwitchReason reason) {
  RTC_DCHECK_RUN_ON(network_thread_);
  SwitchSelectedConnectionInternal(FromIceController(new_connection), reason);
}

// Change the selected connection, and let listeners know.
void P2PTransportChannel::SwitchSelectedConnectionInternal(
    Connection* conn,
    IceSwitchReason reason) {
  RTC_DCHECK_RUN_ON(network_thread_);
  // Note: if conn is NULL, the previous `selected_connection_` has been
  // destroyed, so don't use it.
  Connection* old_selected_connection = selected_connection_;
  selected_connection_ = conn;
  // [新增] 主连接变更后，立即同步次优连接
  UpdateSecondSelectedConnection();
  LogCandidatePairConfig(conn, IceCandidatePairConfigType::kSelected);
  network_route_.reset();
  if (old_selected_connection) {
    old_selected_connection->set_selected(false);
  }
  if (selected_connection_) {
    ++nomination_;
    selected_connection_->set_selected(true);
    if (old_selected_connection) {
      RTC_LOG(LS_INFO) << ToString() << ": Previous selected connection: "
                       << old_selected_connection->ToString();
    }
    RTC_LOG(LS_INFO) << ToString() << ": New selected connection: "
                     << selected_connection_->ToString();
    SignalRouteChange(this, selected_connection_->remote_candidate());
    // This is a temporary, but safe fix to webrtc issue 5705.
    // TODO(honghaiz): Make all ENOTCONN error routed through the transport
    // channel so that it knows whether the media channel is allowed to
    // send; then it will only signal ready-to-send if the media channel
    // has been disallowed to send.
    if (selected_connection_->writable() ||
        PresumedWritable(selected_connection_)) {
      SignalReadyToSend(this);
    }

    network_route_.emplace(ConfigureNetworkRoute(selected_connection_));
  } else {
    RTC_LOG(LS_INFO) << ToString() << ": No selected connection";
  }

  if (conn != nullptr && ice_role_ == ICEROLE_CONTROLLING &&
      ((ice_field_trials_.send_ping_on_switch_ice_controlling &&
        old_selected_connection != nullptr) ||
       ice_field_trials_.send_ping_on_selected_ice_controlling)) {
    SendPingRequestInternal(conn);
  }

  SignalNetworkRouteChanged(network_route_);

  // Create event for candidate pair change.
  if (selected_connection_) {
    CandidatePairChangeEvent pair_change;
    pair_change.reason = IceSwitchReasonToString(reason);
    pair_change.selected_candidate_pair = *GetSelectedCandidatePair();
    pair_change.last_data_received_ms =
        selected_connection_->last_data_received();

    if (old_selected_connection) {
      pair_change.estimated_disconnected_time_ms =
          ComputeEstimatedDisconnectedTimeMs(TimeMillis(),
                                             old_selected_connection);
    } else {
      pair_change.estimated_disconnected_time_ms = 0;
    }
    if (candidate_pair_change_callback_) {
      candidate_pair_change_callback_(pair_change);
    }
  }

  ++selected_candidate_pair_changes_;

  ice_controller_->OnConnectionSwitched(selected_connection_);
}

int64_t P2PTransportChannel::ComputeEstimatedDisconnectedTimeMs(
    int64_t now_ms,
    Connection* old_connection) {
  // TODO(jonaso): nicer keeps estimate of how frequently data _should_ be
  // received, this could be used to give better estimate (if needed).
  int64_t last_data_or_old_ping =
      std::max(old_connection->last_received(), last_data_received_ms_);
  return (now_ms - last_data_or_old_ping);
}

// Warning: UpdateTransportState should eventually be called whenever a
// connection is added, deleted, or the write state of any connection changes so
// that the transport controller will get the up-to-date channel state. However
// it should not be called too often; in the case that multiple connection
// states change, it should be called after all the connection states have
// changed. For example, we call this at the end of
// SortConnectionsAndUpdateState.
void P2PTransportChannel::UpdateTransportState() {
  RTC_DCHECK_RUN_ON(network_thread_);
  // If our selected connection is "presumed writable" (TURN-TURN with no
  // CreatePermission required), act like we're already writable to the upper
  // layers, so they can start media quicker.
  bool writable =
      selected_connection_ && (selected_connection_->writable() ||
                               PresumedWritable(selected_connection_));
  SetWritable(writable);

  bool receiving = false;
  for (const Connection* connection : connections_) {
    if (connection->receiving()) {
      receiving = true;
      break;
    }
  }
  SetReceiving(receiving);

  IceTransportStateInternal state = ComputeState();
  IceTransportState current_standardized_state = ComputeIceTransportState();

  if (state_ != state) {
    RTC_LOG(LS_INFO) << ToString() << ": Transport channel state changed from "
                     << static_cast<int>(state_) << " to "
                     << static_cast<int>(state);
    // Check that the requested transition is allowed. Note that
    // P2PTransportChannel does not (yet) implement a direct mapping of the
    // ICE states from the standard; the difference is covered by
    // TransportController and PeerConnection.
    switch (state_) {
      case IceTransportStateInternal::STATE_INIT:
        // TODO(deadbeef): Once we implement end-of-candidates signaling,
        // we shouldn't go from INIT to COMPLETED.
        RTC_DCHECK(state == IceTransportStateInternal::STATE_CONNECTING ||
                   state == IceTransportStateInternal::STATE_COMPLETED ||
                   state == IceTransportStateInternal::STATE_FAILED);
        break;
      case IceTransportStateInternal::STATE_CONNECTING:
        RTC_DCHECK(state == IceTransportStateInternal::STATE_COMPLETED ||
                   state == IceTransportStateInternal::STATE_FAILED);
        break;
      case IceTransportStateInternal::STATE_COMPLETED:
        // TODO(deadbeef): Once we implement end-of-candidates signaling,
        // we shouldn't go from COMPLETED to CONNECTING.
        // Though we *can* go from COMPlETED to FAILED, if consent expires.
        RTC_DCHECK(state == IceTransportStateInternal::STATE_CONNECTING ||
                   state == IceTransportStateInternal::STATE_FAILED);
        break;
      case IceTransportStateInternal::STATE_FAILED:
        // TODO(deadbeef): Once we implement end-of-candidates signaling,
        // we shouldn't go from FAILED to CONNECTING or COMPLETED.
        RTC_DCHECK(state == IceTransportStateInternal::STATE_CONNECTING ||
                   state == IceTransportStateInternal::STATE_COMPLETED);
        break;
      default:
        RTC_DCHECK_NOTREACHED();
        break;
    }
    state_ = state;
    SignalStateChanged(this);
  }

  if (standardized_state_ != current_standardized_state) {
    standardized_state_ = current_standardized_state;
    SignalIceTransportStateChanged(this);
  }
}

void P2PTransportChannel::MaybeStopPortAllocatorSessions() {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (!IsGettingPorts()) {
    return;
  }

  for (const auto& session : allocator_sessions_) {
    if (session->IsStopped()) {
      continue;
    }
    // If gathering continually, keep the last session running so that
    // it can gather candidates if the networks change.
    if (config_.gather_continually() && session == allocator_sessions_.back()) {
      session->ClearGettingPorts();
    } else {
      session->StopGettingPorts();
    }
  }
}

void P2PTransportChannel::OnSelectedConnectionDestroyed() {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_LOG(LS_INFO) << "Selected connection destroyed. Will choose a new one.";
  IceSwitchReason reason = IceSwitchReason::SELECTED_CONNECTION_DESTROYED;
  SwitchSelectedConnectionInternal(nullptr, reason);
  ice_controller_->OnSortAndSwitchRequest(reason);
}

// If all connections timed out, delete them all.
void P2PTransportChannel::HandleAllTimedOut() {
  RTC_DCHECK_RUN_ON(network_thread_);
  bool update_selected_connection = false;
  std::vector<Connection*> copy(connections_.begin(), connections_.end());
  for (Connection* connection : copy) {
    if (selected_connection_ == connection) {
      selected_connection_ = nullptr;
      update_selected_connection = true;
    }
    connection->SignalDestroyed.disconnect(this);
    RemoveConnection(connection);
    connection->Destroy();
  }

  if (update_selected_connection)
    OnSelectedConnectionDestroyed();
}

bool P2PTransportChannel::ReadyToSend(const Connection* connection) const {
  RTC_DCHECK_RUN_ON(network_thread_);
  // Note that we allow sending on an unreliable connection, because it's
  // possible that it became unreliable simply due to bad chance.
  // So this shouldn't prevent attempting to send media.
  return connection != nullptr &&
         (connection->writable() ||
          connection->write_state() == Connection::STATE_WRITE_UNRELIABLE ||
          PresumedWritable(connection));
}

// This method is only for unit testing.
Connection* P2PTransportChannel::FindNextPingableConnection() {
  RTC_DCHECK_RUN_ON(network_thread_);
  const Connection* conn = ice_controller_->FindNextPingableConnection();
  if (conn) {
    return FromIceController(conn);
  } else {
    return nullptr;
  }
}

int64_t P2PTransportChannel::GetLastPingSentMs() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return last_ping_sent_ms_;
}

void P2PTransportChannel::SendPingRequest(const Connection* connection) {
  RTC_DCHECK_RUN_ON(network_thread_);
  SendPingRequestInternal(FromIceController(connection));
}

void P2PTransportChannel::SendPingRequestInternal(Connection* connection) {
  RTC_DCHECK_RUN_ON(network_thread_);
  PingConnection(connection);
  MarkConnectionPinged(connection);
}

// A connection is considered a backup connection if the channel state
// is completed, the connection is not the selected connection and it is
// active.
void P2PTransportChannel::MarkConnectionPinged(Connection* conn) {
  RTC_DCHECK_RUN_ON(network_thread_);
  ice_controller_->OnConnectionPinged(conn);
}

// Apart from sending ping from `conn` this method also updates
// `use_candidate_attr` and `nomination` flags. One of the flags is set to
// nominate `conn` if this channel is in CONTROLLING.
void P2PTransportChannel::PingConnection(Connection* conn) {
  RTC_DCHECK_RUN_ON(network_thread_);
  bool use_candidate_attr = false;
  uint32_t nomination = 0;
  if (ice_role_ == ICEROLE_CONTROLLING) {
    bool renomination_supported = ice_parameters_.renomination &&
                                  !remote_ice_parameters_.empty() &&
                                  remote_ice_parameters_.back().renomination;
    if (renomination_supported) {
      nomination = GetNominationAttr(conn);
    } else {
      use_candidate_attr = GetUseCandidateAttr(conn);
    }
  }
  conn->set_nomination(nomination);
  conn->set_use_candidate_attr(use_candidate_attr);
  last_ping_sent_ms_ = TimeMillis();
  conn->Ping(last_ping_sent_ms_, stun_dict_writer_.CreateDelta());
}

uint32_t P2PTransportChannel::GetNominationAttr(Connection* conn) const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return (conn == selected_connection_) ? nomination_ : 0;
}

// Nominate a connection based on the NominationMode.
bool P2PTransportChannel::GetUseCandidateAttr(Connection* conn) const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return ice_controller_->GetUseCandidateAttribute(
      conn, config_.default_nomination_mode, remote_ice_mode_);
}

// When a connection's state changes, we need to figure out who to use as
// the selected connection again.  It could have become usable, or become
// unusable.
void P2PTransportChannel::OnConnectionStateChange(Connection* connection) {
  RTC_DCHECK_RUN_ON(network_thread_);

  // May stop the allocator session when at least one connection becomes
  // strongly connected after starting to get ports and the local candidate of
  // the connection is at the latest generation. It is not enough to check
  // that the connection becomes weakly connected because the connection may
  // be changing from (writable, receiving) to (writable, not receiving).
  if (ice_field_trials_.stop_gather_on_strongly_connected) {
    bool strongly_connected = !connection->weak();
    bool latest_generation = connection->local_candidate().generation() >=
                             allocator_session()->generation();
    if (strongly_connected && latest_generation) {
      MaybeStopPortAllocatorSessions();
    }
  }
  // We have to unroll the stack before doing this because we may be changing
  // the state of connections while sorting.
  ice_controller_->OnSortAndSwitchRequest(
      IceSwitchReason::CONNECT_STATE_CHANGE);  // "candidate pair state
                                               // changed");
  // [新增] 排序可能发生了变化（即使没切主路），更新一下次优缓存
  UpdateSecondSelectedConnection();
}

// When a connection is removed, edit it out, and then update our best
// connection.
void P2PTransportChannel::OnConnectionDestroyed(Connection* connection) {
  RTC_DCHECK_RUN_ON(network_thread_);

  // Note: the previous selected_connection_ may be destroyed by now, so don't
  // use it.

  // Remove this connection from the list.
  RemoveConnection(connection);

// [新增] 如果销毁的是当前的次优连接，立即重置并更新
  if (second_selected_connection_ == connection) {
    second_selected_connection_ = nullptr;
    // 尝试找个新的
    UpdateSecondSelectedConnection();
  }
  RTC_LOG(LS_INFO) << ToString() << ": Removed connection " << connection
                   << " (" << connections_.size() << " remaining)";

  // If this is currently the selected connection, then we need to pick a new
  // one. The call to SortConnectionsAndUpdateState will pick a new one. It
  // looks at the current selected connection in order to avoid switching
  // between fairly similar ones. Since this connection is no longer an
  // option, we can just set selected to nullptr and re-choose a best assuming
  // that there was no selected connection.
  if (selected_connection_ == connection) {
    OnSelectedConnectionDestroyed();
  } else {
    // If a non-selected connection was destroyed, we don't need to re-sort but
    // we do need to update state, because we could be switching to "failed" or
    // "completed".
    UpdateTransportState();
  }
}

void P2PTransportChannel::RemoveConnection(Connection* connection) {
  RTC_DCHECK_RUN_ON(network_thread_);
  auto it = absl::c_find(connections_, connection);
  RTC_DCHECK(it != connections_.end());
  connection->DeregisterReceivedPacketCallback();
  connections_.erase(it);
  connection->ClearStunDictConsumer();
  connection->DeregisterDtlsPiggyback();
  ice_controller_->OnConnectionDestroyed(connection);
}

// When a port is destroyed, remove it from our list of ports to use for
// connection attempts.
void P2PTransportChannel::OnPortDestroyed(PortInterface* port) {
  RTC_DCHECK_RUN_ON(network_thread_);

  ports_.erase(std::remove(ports_.begin(), ports_.end(), port), ports_.end());
  pruned_ports_.erase(
      std::remove(pruned_ports_.begin(), pruned_ports_.end(), port),
      pruned_ports_.end());
  RTC_LOG(LS_INFO) << "Removed port because it is destroyed: " << ports_.size()
                   << " remaining";
}

void P2PTransportChannel::OnPortsPruned(
    PortAllocatorSession* /* session */,
    const std::vector<PortInterface*>& ports) {
  RTC_DCHECK_RUN_ON(network_thread_);
  for (PortInterface* port : ports) {
    if (PrunePort(port)) {
      RTC_LOG(LS_INFO) << "Removed port: " << port->ToString() << " "
                       << ports_.size() << " remaining";
    }
  }
}

void P2PTransportChannel::OnCandidatesRemoved(
    PortAllocatorSession* session,
    const std::vector<Candidate>& candidates) {
  RTC_DCHECK_RUN_ON(network_thread_);
  // Do not signal candidate removals if continual gathering is not enabled,
  // or if this is not the last session because an ICE restart would have
  // signaled the remote side to remove all candidates in previous sessions.
  if (!config_.gather_continually() || session != allocator_session()) {
    return;
  }

  std::vector<Candidate> candidates_to_remove;
  for (Candidate candidate : candidates) {
    candidate.set_transport_name(transport_name());
    candidates_to_remove.push_back(candidate);
  }
  if (candidates_removed_callback_) {
    candidates_removed_callback_(this, candidates_to_remove);
  }
}

void P2PTransportChannel::PruneAllPorts() {
  RTC_DCHECK_RUN_ON(network_thread_);
  pruned_ports_.insert(pruned_ports_.end(), ports_.begin(), ports_.end());
  ports_.clear();
}

bool P2PTransportChannel::PrunePort(PortInterface* port) {
  RTC_DCHECK_RUN_ON(network_thread_);
  auto it = absl::c_find(ports_, port);
  // Don't need to do anything if the port has been deleted from the port
  // list.
  if (it == ports_.end()) {
    return false;
  }
  ports_.erase(it);
  pruned_ports_.push_back(port);
  return true;
}

// We data is available, let listeners know
void P2PTransportChannel::OnReadPacket(Connection* connection,
                                       const ReceivedIpPacket& packet) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (connection != selected_connection_ && !FindConnection(connection)) {
    // Do not deliver, if packet doesn't belong to the correct transport
    // channel.
    RTC_DCHECK_NOTREACHED();
    return;
  }

  // Let the client know of an incoming packet
  packets_received_++;
  bytes_received_ += packet.payload().size();
  RTC_DCHECK(connection->last_data_received() >= last_data_received_ms_);
  last_data_received_ms_ =
      std::max(last_data_received_ms_, connection->last_data_received());

  NotifyPacketReceived(packet);

  // May need to switch the sending connection based on the receiving media
  // path if this is the controlled side.
  if (ice_role_ == ICEROLE_CONTROLLED && connection != selected_connection_) {
    const auto payload = packet.payload();
    const bool is_rtp_rtcp = LooksLikeRtpOrRtcp(
        reinterpret_cast<const char*>(payload.data()), payload.size());
    const bool suppress_switch =
        IceMultipathDualPathSendActive() &&
        (!kIceMultipathOnlyRtpRtcp || is_rtp_rtcp);
    if (!suppress_switch) {
      ice_controller_->OnImmediateSwitchRequest(IceSwitchReason::DATA_RECEIVED,
                                                connection);
    }
  }
}

void P2PTransportChannel::OnSentPacket(const SentPacketInfo& sent_packet) {
  RTC_DCHECK_RUN_ON(network_thread_);

  SignalSentPacket(this, sent_packet);
}

void P2PTransportChannel::OnReadyToSend(Connection* connection) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (connection == selected_connection_ && writable()) {
    SignalReadyToSend(this);
  }
}

void P2PTransportChannel::SetWritable(bool writable) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (writable_ == writable) {
    return;
  }
  RTC_LOG(LS_VERBOSE) << ToString() << ": Changed writable_ to " << writable;
  writable_ = writable;
  if (writable_) {
    has_been_writable_ = true;
    SignalReadyToSend(this);
  }
  SignalWritableState(this);
}

void P2PTransportChannel::SetReceiving(bool receiving) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (receiving_ == receiving) {
    return;
  }
  receiving_ = receiving;
  SignalReceivingState(this);
}

Candidate P2PTransportChannel::SanitizeLocalCandidate(
    const Candidate& c) const {
  RTC_DCHECK_RUN_ON(network_thread_);
  // Delegates to the port allocator.
  return allocator_->SanitizeCandidate(c);
}

Candidate P2PTransportChannel::SanitizeRemoteCandidate(
    const Candidate& c) const {
  RTC_DCHECK_RUN_ON(network_thread_);
  // If the remote endpoint signaled us an mDNS candidate, we assume it
  // is supposed to be sanitized.
  bool use_hostname_address = absl::EndsWith(c.address().hostname(), LOCAL_TLD);
  // Remove the address for prflx remote candidates. See
  // https://w3c.github.io/webrtc-stats/#dom-rtcicecandidatestats.
  use_hostname_address |= c.is_prflx();
  // Filter remote ufrag of peer-reflexive candidates before any ICE parameters
  // are known.
  uint32_t remote_generation = 0;
  bool filter_ufrag =
      c.is_prflx() &&
      FindRemoteIceFromUfrag(c.username(), &remote_generation) == nullptr;
  return c.ToSanitizedCopy(use_hostname_address,
                           false /* filter_related_address */, filter_ufrag);
}

void P2PTransportChannel::LogCandidatePairConfig(
    Connection* conn,
    IceCandidatePairConfigType type) {
  RTC_DCHECK_RUN_ON(network_thread_);
  if (conn == nullptr) {
    return;
  }
  ice_event_log_.LogCandidatePairConfig(type, conn->id(),
                                        conn->ToLogDescription());
}

std::unique_ptr<StunAttribute> P2PTransportChannel::GoogDeltaReceived(
    const StunByteStringAttribute* delta) {
  auto error = stun_dict_view_.ApplyDelta(*delta);
  if (error.ok()) {
    auto& result = error.value();
    RTC_LOG(LS_INFO) << "Applied GOOG_DELTA";
    dictionary_view_updated_callback_list_.Send(this, stun_dict_view_,
                                                result.second);
    return std::move(result.first);
  } else {
    RTC_LOG(LS_ERROR) << "Failed to apply GOOG_DELTA: "
                      << error.error().message();
  }
  return nullptr;
}

void P2PTransportChannel::GoogDeltaAckReceived(
    RTCErrorOr<const StunUInt64Attribute*> error_or_ack) {
  if (error_or_ack.ok()) {
    RTC_LOG(LS_ERROR) << "Applied GOOG_DELTA_ACK";
    auto ack = error_or_ack.value();
    stun_dict_writer_.ApplyDeltaAck(*ack);
    dictionary_writer_synced_callback_list_.Send(this, stun_dict_writer_);
  } else {
    stun_dict_writer_.Disable();
    RTC_LOG(LS_ERROR) << "Failed GOOG_DELTA_ACK: "
                      << error_or_ack.error().message();
  }
}

void P2PTransportChannel::SetDtlsStunPiggybackCallbacks(
    DtlsStunPiggybackCallbacks&& callbacks) {
  RTC_DCHECK_RUN_ON(network_thread_);
  RTC_DCHECK(connections_.empty());
  RTC_DCHECK(!callbacks.empty());
  dtls_stun_piggyback_callbacks_ = std::move(callbacks);
}

void P2PTransportChannel::ResetDtlsStunPiggybackCallbacks() {
  RTC_DCHECK_RUN_ON(network_thread_);
  dtls_stun_piggyback_callbacks_.reset();
  for (auto& connection : connections_) {
    connection->DeregisterDtlsPiggyback();
  }
}

size_t P2PTransportChannel::PermissionQueriesOutstandingForTesting() const {
  RTC_DCHECK_RUN_ON(network_thread_);
  return permission_queries_.size();
}

}  // namespace webrtc
