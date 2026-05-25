#ifndef RTC_BASE_NETWORK_ROUTE_PROBE_BITRATE_TRACKER_H_
#define RTC_BASE_NETWORK_ROUTE_PROBE_BITRATE_TRACKER_H_

#include <cstdint>
#include <optional>

namespace webrtc {

enum class WebrtcMultipathSendMode : uint8_t {
  // Stock WebRTC: no ICE multipath send, no per-path probe store, no pacer
  // path hints.
  kNative = 0,
  // Multipath path hints with GCC-ratio media splitting and best-path VIP
  // forwarding (this tree's default experiment).
  kExtended = 1,
  // Send each eligible packet on primary or secondary in strict alternation
  // (no quota hints; probes still tracked when not kNative).
  kRoundRobin = 2,
  // Single ICE send path, but still publish multipath hints so pacer path
  // selection remains active (secondary mirrors primary estimates).
  kSinglePathPacerPriority = 3,
};

// Defaults to kExtended. Call from the embedder before creating peer connections.
void SetWebrtcMultipathSendMode(WebrtcMultipathSendMode mode);
WebrtcMultipathSendMode GetWebrtcMultipathSendMode();

// True for kExtended or kRoundRobin (anything that uses multipath hooks).
bool IsWebrtcMultipathExtensionEnabled();
inline bool IsWebrtcMultipathRoundRobinMode() {
  return GetWebrtcMultipathSendMode() == WebrtcMultipathSendMode::kRoundRobin;
}

// True when ICE may use two live send paths (extended or round-robin).
inline bool IsWebrtcMultipathIceMultipathSendActive() {
  const WebrtcMultipathSendMode m = GetWebrtcMultipathSendMode();
  return m == WebrtcMultipathSendMode::kExtended ||
         m == WebrtcMultipathSendMode::kRoundRobin;
}

inline bool IsSinglePathPacerPriorityMode() {
  return GetWebrtcMultipathSendMode() ==
         WebrtcMultipathSendMode::kSinglePathPacerPriority;
}

// Convenience: maps to kExtended / kNative.
void SetWebrtcMultipathExtensionEnabled(bool enabled);

struct MultipathQuotaHint {
  uint32_t primary_estimated_bps = 0;
  uint32_t secondary_estimated_bps = 0;
  int schedule_window_ms = 0;
};

// Latest delay-based (GCC) target rate per local network_id (send path).
void UpdateDelayBasedBitrateForNetwork(uint16_t network_id,
                                       uint32_t bitrate_bps);
std::optional<uint32_t> GetDelayBasedBitrateForNetwork(uint16_t network_id);
void ClearDelayBasedBitrateMap();

// Stores latest probe-estimated bitrate per local network_id.
void UpdateProbeBitrateForNetwork(uint16_t network_id, uint32_t bitrate_bps);
std::optional<uint32_t> GetProbeBitrateForNetwork(uint16_t network_id);
void RequestGccProbeOnNewPath();
bool ConsumeGccProbeRequest();
void UpdateMultipathQuotaHint(const MultipathQuotaHint& hint);
std::optional<MultipathQuotaHint> GetMultipathQuotaHint();

}  // namespace webrtc

#endif  // RTC_BASE_NETWORK_ROUTE_PROBE_BITRATE_TRACKER_H_
