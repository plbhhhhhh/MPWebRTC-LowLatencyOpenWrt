/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/nack_requester.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "api/field_trials_view.h"
#include "api/sequence_checker.h"
#include "api/task_queue/task_queue_base.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "modules/include/module_common_types.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/network_route_probe_bitrate_tracker.h"
#include "rtc_base/numerics/mod_ops.h"
#include "rtc_base/numerics/sequence_number_util.h"
#include "rtc_base/task_utils/repeating_task.h"
#include "system_wrappers/include/clock.h"

namespace webrtc {

namespace {
constexpr int kMaxPacketAge = 10'000;
constexpr int kMaxNackPackets = 1000;
constexpr int kMaxNackPacketsMultipath = 5000;
constexpr int kMaxNackPacketsRoundRobin = 1000;
constexpr TimeDelta kDefaultRtt = TimeDelta::Millis(100);
// Number of times a packet can be nacked before giving up. Nack is sent at most
// every RTT.
constexpr int kMaxNackRetries = 100;
// Multipath reordering depth can be significantly larger than single-path
// jitter. Keep histogram range wide so wait-packet estimation does not clip.
constexpr int kMaxReorderedPackets = 512;
constexpr int kNumReorderingBuckets = 16;
constexpr TimeDelta kDefaultSendNackDelay = TimeDelta::Zero();

// When the multipath extension is enabled, packets belonging to the same stream
// can arrive via paths with noticeably different one-way delays, so the
// receiver must tolerate out-of-order arrival before concluding a gap is a true
// loss. Since we intentionally allow larger receive playout delay in multipath
// mode, use a longer default NACK hold-back to reduce false positives.
constexpr TimeDelta kMultipathDefaultSendNackDelay = TimeDelta::Millis(260);
// Strict alternation across two paths tends to produce deeper RTP reordering than
// quota-based scheduling; wait longer before NACK to reduce false positives.
constexpr TimeDelta kRoundRobinDefaultSendNackDelay = TimeDelta::Zero();
constexpr int kMultipathFieldTrialMaxDelayMs = 500;
constexpr int kSinglepathFieldTrialMaxDelayMs = 20;
constexpr float kSinglePathReorderingWaitProbability = 0.5f;
constexpr float kMultipathReorderingWaitProbability = 0.998f;
constexpr int kMultipathMinReorderingWaitPackets = 32;
constexpr int kRoundRobinMinReorderingWaitPackets = 0;

TimeDelta GetSendNackDelay(const FieldTrialsView& field_trials) {
  const bool round_robin = IsWebrtcMultipathRoundRobinMode();
  const bool multipath = IsWebrtcMultipathExtensionEnabled() && !round_robin;
  const int max_ms = multipath ? kMultipathFieldTrialMaxDelayMs
                               : kSinglepathFieldTrialMaxDelayMs;
  int64_t delay_ms = strtol(
      field_trials.Lookup("WebRTC-SendNackDelayMs").c_str(), nullptr, 10);
  if (delay_ms > 0 && delay_ms <= max_ms) {
    RTC_LOG(LS_INFO) << "SendNackDelay is set to " << delay_ms << "ms"
                     << " (multipath=" << multipath
                     << ", round_robin=" << round_robin << ")";
    return TimeDelta::Millis(delay_ms);
  }
  if (round_robin) {
    RTC_LOG(LS_INFO) << "SendNackDelay using round-robin multipath default "
                     << kRoundRobinDefaultSendNackDelay.ms() << "ms";
    return kRoundRobinDefaultSendNackDelay;
  }
  if (multipath) {
    RTC_LOG(LS_INFO) << "SendNackDelay using multipath default "
                     << kMultipathDefaultSendNackDelay.ms() << "ms";
    return kMultipathDefaultSendNackDelay;
  }
  return kDefaultSendNackDelay;
}

size_t MaxNackPacketsForCurrentMode() {
  if (IsWebrtcMultipathRoundRobinMode()) {
    return kMaxNackPacketsRoundRobin;
  }
  if (IsWebrtcMultipathExtensionEnabled()) {
    return kMaxNackPacketsMultipath;
  }
  return kMaxNackPackets;
}
}  // namespace

constexpr TimeDelta NackPeriodicProcessor::kUpdateInterval;

NackPeriodicProcessor::NackPeriodicProcessor(TimeDelta update_interval)
    : update_interval_(update_interval) {}

NackPeriodicProcessor::~NackPeriodicProcessor() {}

void NackPeriodicProcessor::RegisterNackModule(NackRequesterBase* module) {
  RTC_DCHECK_RUN_ON(&sequence_);
  modules_.push_back(module);
  if (modules_.size() != 1)
    return;
  repeating_task_ = RepeatingTaskHandle::DelayedStart(
      TaskQueueBase::Current(), update_interval_, [this] {
        RTC_DCHECK_RUN_ON(&sequence_);
        ProcessNackModules();
        return update_interval_;
      });
}

void NackPeriodicProcessor::UnregisterNackModule(NackRequesterBase* module) {
  RTC_DCHECK_RUN_ON(&sequence_);
  auto it = std::find(modules_.begin(), modules_.end(), module);
  RTC_DCHECK(it != modules_.end());
  modules_.erase(it);
  if (modules_.empty())
    repeating_task_.Stop();
}

void NackPeriodicProcessor::ProcessNackModules() {
  RTC_DCHECK_RUN_ON(&sequence_);
  for (NackRequesterBase* module : modules_)
    module->ProcessNacks();
}

ScopedNackPeriodicProcessorRegistration::
    ScopedNackPeriodicProcessorRegistration(NackRequesterBase* module,
                                            NackPeriodicProcessor* processor)
    : module_(module), processor_(processor) {
  processor_->RegisterNackModule(module_);
}

ScopedNackPeriodicProcessorRegistration::
    ~ScopedNackPeriodicProcessorRegistration() {
  processor_->UnregisterNackModule(module_);
}

NackRequester::NackInfo::NackInfo()
    : seq_num(0),
      send_at_seq_num(0),
      created_at_time(Timestamp::MinusInfinity()),
      sent_at_time(Timestamp::MinusInfinity()),
      retries(0) {}

NackRequester::NackInfo::NackInfo(uint16_t seq_num,
                                  uint16_t send_at_seq_num,
                                  Timestamp created_at_time)
    : seq_num(seq_num),
      send_at_seq_num(send_at_seq_num),
      created_at_time(created_at_time),
      sent_at_time(Timestamp::MinusInfinity()),
      retries(0) {}

NackRequester::NackRequester(TaskQueueBase* current_queue,
                             NackPeriodicProcessor* periodic_processor,
                             Clock* clock,
                             NackSender* nack_sender,
                             KeyFrameRequestSender* keyframe_request_sender,
                             const FieldTrialsView& field_trials)
    : worker_thread_(current_queue),
      clock_(clock),
      nack_sender_(nack_sender),
      keyframe_request_sender_(keyframe_request_sender),
      reordering_histogram_(kNumReorderingBuckets, kMaxReorderedPackets),
      initialized_(false),
      rtt_(kDefaultRtt),
      newest_seq_num_(0),
      send_nack_delay_(GetSendNackDelay(field_trials)),
      multipath_mode_(IsWebrtcMultipathExtensionEnabled()),
      round_robin_mode_(IsWebrtcMultipathRoundRobinMode()),
      max_nack_packets_(MaxNackPacketsForCurrentMode()),
      processor_registration_(this, periodic_processor) {
  RTC_DCHECK(clock_);
  RTC_DCHECK(nack_sender_);
  RTC_DCHECK(keyframe_request_sender_);
  RTC_DCHECK(worker_thread_);
  RTC_DCHECK(worker_thread_->IsCurrent());
  RTC_LOG(LS_INFO) << "NACK requester mode: multipath=" << multipath_mode_
                   << ", round_robin=" << round_robin_mode_
                   << ", send_nack_delay_ms=" << send_nack_delay_.ms()
                   << ", max_nack_packets=" << max_nack_packets_;
}

NackRequester::~NackRequester() {
  RTC_DCHECK_RUN_ON(worker_thread_);
}

void NackRequester::ProcessNacks() {
  RTC_DCHECK_RUN_ON(worker_thread_);
  std::vector<uint16_t> nack_batch = GetNackBatch(kTimeOnly);
  if (!nack_batch.empty()) {
    // This batch of NACKs is triggered externally; there is no external
    // initiator who can batch them with other feedback messages.
    nack_sender_->SendNack(nack_batch, /*buffering_allowed=*/false);
  }
}

int NackRequester::OnReceivedPacket(uint16_t seq_num) {
  RTC_DCHECK_RUN_ON(worker_thread_);
  return OnReceivedPacket(seq_num, false);
}

int NackRequester::OnReceivedPacket(uint16_t seq_num, bool is_recovered) {
  RTC_DCHECK_RUN_ON(worker_thread_);
  // Only extended multipath mode keeps extra late-packet tolerance for
  // reordering adaptation. Round-robin follows single-path behavior.
  const bool use_multipath_reordering_tolerance =
      multipath_mode_ && !round_robin_mode_;
  bool is_retransmitted = !use_multipath_reordering_tolerance;

  if (!initialized_) {
    newest_seq_num_ = seq_num;
    initialized_ = true;
    return 0;
  }

  // Since the `newest_seq_num_` is a packet we have actually received we know
  // that packet has never been Nacked.
  if (seq_num == newest_seq_num_)
    return 0;

  if (AheadOf(newest_seq_num_, seq_num)) {
    // An out of order packet has been received.
    auto nack_list_it = nack_list_.find(seq_num);
    int nacks_sent_for_packet = 0;
    if (nack_list_it != nack_list_.end()) {
      nacks_sent_for_packet = nack_list_it->second.retries;
      nack_list_.erase(nack_list_it);
    }
    if (!is_retransmitted)
      UpdateReorderingStatistics(seq_num);
    return nacks_sent_for_packet;
  }

  if (is_recovered) {
    recovered_list_.insert(seq_num);

    // Remove old ones so we don't accumulate recovered packets.
    auto it = recovered_list_.lower_bound(seq_num - kMaxPacketAge);
    if (it != recovered_list_.begin())
      recovered_list_.erase(recovered_list_.begin(), it);

    // Do not send nack for packets recovered by FEC or RTX.
    return 0;
  }

  AddPacketsToNack(newest_seq_num_ + 1, seq_num);
  newest_seq_num_ = seq_num;

  // Are there any nacks that are waiting for this seq_num.
  std::vector<uint16_t> nack_batch = GetNackBatch(kSeqNumOnly);
  if (!nack_batch.empty()) {
    // This batch of NACKs is triggered externally; the initiator can
    // batch them with other feedback messages.
    nack_sender_->SendNack(nack_batch, /*buffering_allowed=*/true);
  }

  return 0;
}

void NackRequester::ClearUpTo(uint16_t seq_num) {
  // TODO(bugs.webrtc.org/11993): This method is actually called on the worker
  // thread even though the caller stack to this call passes thread checkers
  // indicating they belong to the network thread. The inline execution below
  // needs to be posted to the worker thread if callers migrate to the network
  // thread.
  RTC_DCHECK_RUN_ON(worker_thread_);
  nack_list_.erase(nack_list_.begin(), nack_list_.lower_bound(seq_num));
  recovered_list_.erase(recovered_list_.begin(),
                        recovered_list_.lower_bound(seq_num));
}

void NackRequester::UpdateRtt(int64_t rtt_ms) {
  RTC_DCHECK_RUN_ON(worker_thread_);
  rtt_ = TimeDelta::Millis(rtt_ms);
}

void NackRequester::AddPacketsToNack(uint16_t seq_num_start,
                                     uint16_t seq_num_end) {
  // Called on worker_thread_.
  // Remove old packets.
  auto it = nack_list_.lower_bound(seq_num_end - kMaxPacketAge);
  nack_list_.erase(nack_list_.begin(), it);

  uint16_t num_new_nacks = ForwardDiff(seq_num_start, seq_num_end);
  if (nack_list_.size() + num_new_nacks > max_nack_packets_) {
    nack_list_.clear();
    RTC_LOG(LS_WARNING) << "NACK list full, clearing NACK"
                           " list and requesting keyframe.";
    keyframe_request_sender_->RequestKeyFrame();
    return;
  }

  for (uint16_t seq_num = seq_num_start; seq_num != seq_num_end; ++seq_num) {
    // Do not send nack for packets that are already recovered by FEC or RTX
    if (recovered_list_.find(seq_num) != recovered_list_.end())
      continue;
    NackInfo nack_info(seq_num, seq_num + GetReorderingWaitPackets(),
                       clock_->CurrentTime());
    RTC_DCHECK(nack_list_.find(seq_num) == nack_list_.end());
    nack_list_[seq_num] = nack_info;
  }
}

std::vector<uint16_t> NackRequester::GetNackBatch(NackFilterOptions options) {
  // Called on worker_thread_.

  bool consider_seq_num = options != kTimeOnly;
  bool consider_timestamp = options != kSeqNumOnly;
  Timestamp now = clock_->CurrentTime();
  std::vector<uint16_t> nack_batch;
  auto it = nack_list_.begin();
  while (it != nack_list_.end()) {
    bool delay_timed_out = now - it->second.created_at_time >= send_nack_delay_;
    bool nack_on_rtt_passed = now - it->second.sent_at_time >= rtt_;
    bool nack_on_seq_num_passed =
        it->second.sent_at_time.IsInfinite() &&
        AheadOrAt(newest_seq_num_, it->second.send_at_seq_num);
    if (delay_timed_out && ((consider_seq_num && nack_on_seq_num_passed) ||
                            (consider_timestamp && nack_on_rtt_passed))) {
      nack_batch.emplace_back(it->second.seq_num);
      ++it->second.retries;
      it->second.sent_at_time = now;
      if (it->second.retries >= kMaxNackRetries) {
        RTC_LOG(LS_WARNING) << "Sequence number " << it->second.seq_num
                            << " removed from NACK list due to max retries.";
        it = nack_list_.erase(it);
      } else {
        ++it;
      }
      continue;
    }
    ++it;
  }
  return nack_batch;
}

void NackRequester::UpdateReorderingStatistics(uint16_t seq_num) {
  // Running on worker_thread_.
  RTC_DCHECK(AheadOf(newest_seq_num_, seq_num));
  uint16_t diff = ReverseDiff(newest_seq_num_, seq_num);
  reordering_histogram_.Add(diff);
}

int NackRequester::WaitNumberOfPackets(float probability) const {
  // Called on worker_thread_;
  if (reordering_histogram_.NumValues() == 0)
    return 0;
  return reordering_histogram_.InverseCdf(probability);
}

int NackRequester::GetReorderingWaitPackets() const {
  const bool use_multipath_tolerance = multipath_mode_ && !round_robin_mode_;
  const float wait_probability = use_multipath_tolerance
                                     ? kMultipathReorderingWaitProbability
                                     : kSinglePathReorderingWaitProbability;
  int wait_packets = WaitNumberOfPackets(wait_probability);
  if (round_robin_mode_) {
    return std::max(wait_packets, kRoundRobinMinReorderingWaitPackets);
  }
  if (multipath_mode_) {
    return std::max(wait_packets, kMultipathMinReorderingWaitPackets);
  }
  return wait_packets;
}

}  // namespace webrtc
