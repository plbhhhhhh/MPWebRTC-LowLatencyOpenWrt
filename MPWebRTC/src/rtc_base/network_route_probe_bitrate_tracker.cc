#include "rtc_base/network_route_probe_bitrate_tracker.h"

#include <atomic>
#include <map>

#include "rtc_base/synchronization/mutex.h"

namespace webrtc {
namespace {

struct ProbeBitrateStore {
  Mutex mutex;
  std::map<uint16_t, uint32_t> bitrate_bps_by_network_id;
  std::map<uint16_t, uint32_t> delay_based_bps_by_network_id;
  std::optional<MultipathQuotaHint> multipath_quota_hint;
};

ProbeBitrateStore& GlobalStore() {
  static ProbeBitrateStore* store = new ProbeBitrateStore();
  return *store;
}

std::atomic<int>& PendingGccProbeRequests() {
  static std::atomic<int> pending_requests{0};
  return pending_requests;
}

std::atomic<uint8_t>& MultipathSendModeStorage() {
  static std::atomic<uint8_t> mode{static_cast<uint8_t>(
      WebrtcMultipathSendMode::kExtended)};
  return mode;
}

}  // namespace

void SetWebrtcMultipathSendMode(WebrtcMultipathSendMode mode) {
  MultipathSendModeStorage().store(static_cast<uint8_t>(mode),
                                   std::memory_order_relaxed);
  ProbeBitrateStore& store = GlobalStore();
  MutexLock lock(&store.mutex);
  store.multipath_quota_hint.reset();
  if (mode == WebrtcMultipathSendMode::kNative) {
    PendingGccProbeRequests().store(0, std::memory_order_relaxed);
    store.bitrate_bps_by_network_id.clear();
    store.delay_based_bps_by_network_id.clear();
  }
}

WebrtcMultipathSendMode GetWebrtcMultipathSendMode() {
  return static_cast<WebrtcMultipathSendMode>(
      MultipathSendModeStorage().load(std::memory_order_relaxed));
}

bool IsWebrtcMultipathExtensionEnabled() {
  return GetWebrtcMultipathSendMode() != WebrtcMultipathSendMode::kNative;
}

void SetWebrtcMultipathExtensionEnabled(bool enabled) {
  SetWebrtcMultipathSendMode(enabled ? WebrtcMultipathSendMode::kExtended
                                     : WebrtcMultipathSendMode::kNative);
}

void UpdateDelayBasedBitrateForNetwork(uint16_t network_id,
                                       uint32_t bitrate_bps) {
  if (!IsWebrtcMultipathExtensionEnabled()) {
    return;
  }
  ProbeBitrateStore& store = GlobalStore();
  MutexLock lock(&store.mutex);
  store.delay_based_bps_by_network_id[network_id] = bitrate_bps;
}

std::optional<uint32_t> GetDelayBasedBitrateForNetwork(
    uint16_t network_id) {
  if (!IsWebrtcMultipathExtensionEnabled()) {
    return std::nullopt;
  }
  ProbeBitrateStore& store = GlobalStore();
  MutexLock lock(&store.mutex);
  auto it = store.delay_based_bps_by_network_id.find(network_id);
  if (it == store.delay_based_bps_by_network_id.end()) {
    return std::nullopt;
  }
  return it->second;
}

void ClearDelayBasedBitrateMap() {
  ProbeBitrateStore& store = GlobalStore();
  MutexLock lock(&store.mutex);
  store.delay_based_bps_by_network_id.clear();
}

void UpdateProbeBitrateForNetwork(uint16_t network_id, uint32_t bitrate_bps) {
  if (!IsWebrtcMultipathExtensionEnabled()) {
    return;
  }
  ProbeBitrateStore& store = GlobalStore();
  MutexLock lock(&store.mutex);
  store.bitrate_bps_by_network_id[network_id] = bitrate_bps;
}

std::optional<uint32_t> GetProbeBitrateForNetwork(uint16_t network_id) {
  if (!IsWebrtcMultipathExtensionEnabled()) {
    return std::nullopt;
  }
  ProbeBitrateStore& store = GlobalStore();
  MutexLock lock(&store.mutex);
  auto it = store.bitrate_bps_by_network_id.find(network_id);
  if (it == store.bitrate_bps_by_network_id.end()) {
    return std::nullopt;
  }
  return it->second;
}

void RequestGccProbeOnNewPath() {
  if (!IsWebrtcMultipathExtensionEnabled()) {
    return;
  }
  PendingGccProbeRequests().fetch_add(1, std::memory_order_relaxed);
}

bool ConsumeGccProbeRequest() {
  if (!IsWebrtcMultipathExtensionEnabled()) {
    return false;
  }
  int current = PendingGccProbeRequests().load(std::memory_order_relaxed);
  while (current > 0) {
    if (PendingGccProbeRequests().compare_exchange_weak(
            current, current - 1, std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

void UpdateMultipathQuotaHint(const MultipathQuotaHint& hint) {
  ProbeBitrateStore& store = GlobalStore();
  MutexLock lock(&store.mutex);
  if (GetWebrtcMultipathSendMode() != WebrtcMultipathSendMode::kExtended &&
      GetWebrtcMultipathSendMode() !=
          WebrtcMultipathSendMode::kSinglePathPacerPriority) {
    store.multipath_quota_hint.reset();
    return;
  }
  if (hint.primary_estimated_bps == 0 || hint.secondary_estimated_bps == 0 ||
      hint.schedule_window_ms <= 0) {
    store.multipath_quota_hint.reset();
    return;
  }
  store.multipath_quota_hint = hint;
}

std::optional<MultipathQuotaHint> GetMultipathQuotaHint() {
  if (GetWebrtcMultipathSendMode() != WebrtcMultipathSendMode::kExtended &&
      GetWebrtcMultipathSendMode() !=
          WebrtcMultipathSendMode::kSinglePathPacerPriority) {
    return std::nullopt;
  }
  ProbeBitrateStore& store = GlobalStore();
  MutexLock lock(&store.mutex);
  return store.multipath_quota_hint;
}

}  // namespace webrtc
