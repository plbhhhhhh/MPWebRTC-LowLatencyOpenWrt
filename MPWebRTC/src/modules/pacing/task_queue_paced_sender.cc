/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/pacing/task_queue_paced_sender.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "api/field_trials_view.h"
#include "api/sequence_checker.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "api/task_queue/task_queue_base.h"
#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "modules/pacing/pacing_controller.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "rtc_base/checks.h"
#include "rtc_base/network_route_probe_bitrate_tracker.h"
#include "rtc_base/numerics/exp_filter.h"
#include "rtc_base/trace_event.h"

namespace webrtc {
namespace {
constexpr int64_t kRecentPathLoadEwmaNumerator = 31;
constexpr int64_t kRecentPathLoadEwmaDenominator = 32;
constexpr int64_t kRecentPathLoadUnit = 1024;
constexpr int64_t kCompensationHysteresis = 32;

size_t GetPacketSchedulingSizeBytes(const RtpPacketToSend& packet,
                                    bool include_overhead) {
  return packet.payload_size() + packet.padding_size() +
         (include_overhead ? packet.headers_size() : 0);
}
}  // namespace

const int TaskQueuePacedSender::kNoPacketHoldback = -1;

TaskQueuePacedSender::TaskQueuePacedSender(
    Clock* clock,
    PacingController::PacketSender* packet_sender,
    const FieldTrialsView& field_trials,
    TimeDelta max_hold_back_window,
    int max_hold_back_window_in_packets)
    : clock_(clock),
      max_hold_back_window_(max_hold_back_window),
      max_hold_back_window_in_packets_(max_hold_back_window_in_packets),
      pacing_controller_(clock, packet_sender, field_trials),
      next_process_time_(Timestamp::MinusInfinity()),
      is_started_(false),
      is_shutdown_(false),
      packet_size_(/*alpha=*/0.95),
      include_overhead_(false),
      task_queue_(TaskQueueBase::Current()) {
  RTC_DCHECK_GE(max_hold_back_window_, PacingController::kMinSleepTime);
}

TaskQueuePacedSender::~TaskQueuePacedSender() {
  RTC_DCHECK_RUN_ON(task_queue_);
  is_shutdown_ = true;
}

void TaskQueuePacedSender::SetSendBurstInterval(TimeDelta burst_interval) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.SetSendBurstInterval(burst_interval);
}

void TaskQueuePacedSender::SetAllowProbeWithoutMediaPacket(bool allow) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.SetAllowProbeWithoutMediaPacket(allow);
}

void TaskQueuePacedSender::EnsureStarted() {
  RTC_DCHECK_RUN_ON(task_queue_);
  is_started_ = true;
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::CreateProbeClusters(
    std::vector<ProbeClusterConfig> probe_cluster_configs) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.CreateProbeClusters(probe_cluster_configs);
  MaybeScheduleProcessPackets();
}

void TaskQueuePacedSender::Pause() {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.Pause();
}

void TaskQueuePacedSender::Resume() {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.Resume();
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::SetCongested(bool congested) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.SetCongested(congested);
  MaybeScheduleProcessPackets();
}

void TaskQueuePacedSender::SetPacingRates(DataRate pacing_rate,
                                          DataRate padding_rate) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.SetPacingRates(pacing_rate, padding_rate);
  MaybeScheduleProcessPackets();
}

void TaskQueuePacedSender::EnqueuePackets(
    std::vector<std::unique_ptr<RtpPacketToSend>> packets) {
  task_queue_->PostTask(
      SafeTask(safety_.flag(), [this, packets = std::move(packets)]() mutable {
        RTC_DCHECK_RUN_ON(task_queue_);
        TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("webrtc"),
                     "TaskQueuePacedSender::EnqueuePackets");
        const bool round_robin_mode = IsWebrtcMultipathRoundRobinMode();
        for (auto& packet : packets) {
          TRACE_EVENT2(TRACE_DISABLED_BY_DEFAULT("webrtc"),
                       "TaskQueuePacedSender::EnqueuePackets::Loop",
                       "sequence_number", packet->SequenceNumber(),
                       "rtp_timestamp", packet->Timestamp());

          size_t packet_size =
              GetPacketSchedulingSizeBytes(*packet, include_overhead_);
          packet_size_.Apply(1, packet_size);
          RTC_DCHECK_GE(packet->capture_time(), Timestamp::Zero());
          if (!round_robin_mode) {
            AssignPathHintForPacket(*packet);
          }
          pacing_controller_.EnqueuePacket(std::move(packet));
        }
        MaybeProcessPackets(Timestamp::MinusInfinity());
      }));
}

void TaskQueuePacedSender::RemovePacketsForSsrc(uint32_t ssrc) {
  task_queue_->PostTask(SafeTask(safety_.flag(), [this, ssrc] {
    RTC_DCHECK_RUN_ON(task_queue_);
    pacing_controller_.RemovePacketsForSsrc(ssrc);
    MaybeProcessPackets(Timestamp::MinusInfinity());
  }));
}

void TaskQueuePacedSender::SetAccountForAudioPackets(bool account_for_audio) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.SetAccountForAudioPackets(account_for_audio);
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::SetIncludeOverhead() {
  RTC_DCHECK_RUN_ON(task_queue_);
  include_overhead_ = true;
  pacing_controller_.SetIncludeOverhead();
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::SetTransportOverhead(DataSize overhead_per_packet) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.SetTransportOverhead(overhead_per_packet);
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::SetQueueTimeLimit(TimeDelta limit) {
  RTC_DCHECK_RUN_ON(task_queue_);
  pacing_controller_.SetQueueTimeLimit(limit);
  MaybeProcessPackets(Timestamp::MinusInfinity());
}

TimeDelta TaskQueuePacedSender::ExpectedQueueTime() const {
  return GetStats().expected_queue_time;
}

DataSize TaskQueuePacedSender::QueueSizeData() const {
  return GetStats().queue_size;
}

std::optional<Timestamp> TaskQueuePacedSender::FirstSentPacketTime() const {
  return GetStats().first_sent_packet_time;
}

TimeDelta TaskQueuePacedSender::OldestPacketWaitTime() const {
  Timestamp oldest_packet = GetStats().oldest_packet_enqueue_time;
  if (oldest_packet.IsInfinite()) {
    return TimeDelta::Zero();
  }

  // (webrtc:9716): The clock is not always monotonic.
  Timestamp current = clock_->CurrentTime();
  if (current < oldest_packet) {
    return TimeDelta::Zero();
  }

  return current - oldest_packet;
}

void TaskQueuePacedSender::OnStatsUpdated(const Stats& stats) {
  RTC_DCHECK_RUN_ON(task_queue_);
  current_stats_ = stats;
}

// RTC_RUN_ON(task_queue_)
void TaskQueuePacedSender::MaybeScheduleProcessPackets() {
  if (!processing_packets_)
    MaybeProcessPackets(Timestamp::MinusInfinity());
}

void TaskQueuePacedSender::MaybeProcessPackets(
    Timestamp scheduled_process_time) {
  RTC_DCHECK_RUN_ON(task_queue_);

  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("webrtc"),
               "TaskQueuePacedSender::MaybeProcessPackets");

  if (is_shutdown_ || !is_started_) {
    return;
  }

  // Protects against re-entry from transport feedback calling into the task
  // queue pacer.
  RTC_DCHECK(!processing_packets_);
  processing_packets_ = true;
  absl::Cleanup cleanup = [this] {
    RTC_DCHECK_RUN_ON(task_queue_);
    processing_packets_ = false;
  };

  const Timestamp now = clock_->CurrentTime();

  Timestamp next_send_time = pacing_controller_.NextSendTime();
  RTC_DCHECK(next_send_time.IsFinite());
  TimeDelta early_execute_margin =
      pacing_controller_.IsProbing()
          ? PacingController::kMaxEarlyProbeProcessing
          : TimeDelta::Zero();

  // Process packets and update stats.
  while (next_send_time <= now + early_execute_margin) {
    pacing_controller_.ProcessPackets();
    next_send_time = pacing_controller_.NextSendTime();
    RTC_DCHECK(next_send_time.IsFinite());

    // Probing state could change. Get margin after process packets.
    early_execute_margin = pacing_controller_.IsProbing()
                               ? PacingController::kMaxEarlyProbeProcessing
                               : TimeDelta::Zero();
  }
  UpdateStats();

  // Ignore retired scheduled task, otherwise reset `next_process_time_`.
  if (scheduled_process_time.IsFinite()) {
    if (scheduled_process_time != next_process_time_) {
      return;
    }
    next_process_time_ = Timestamp::MinusInfinity();
  }

  // Do not hold back in probing.
  TimeDelta hold_back_window = TimeDelta::Zero();
  if (!pacing_controller_.IsProbing()) {
    hold_back_window = max_hold_back_window_;
    DataRate pacing_rate = pacing_controller_.pacing_rate();
    if (max_hold_back_window_in_packets_ != kNoPacketHoldback &&
        !pacing_rate.IsZero() &&
        packet_size_.filtered() != ExpFilter::kValueUndefined) {
      TimeDelta avg_packet_send_time =
          DataSize::Bytes(packet_size_.filtered()) / pacing_rate;
      hold_back_window =
          std::min(hold_back_window,
                   avg_packet_send_time * max_hold_back_window_in_packets_);
    }
  }

  // Calculate next process time.
  TimeDelta time_to_next_process =
      std::max(hold_back_window, next_send_time - now - early_execute_margin);
  next_send_time = now + time_to_next_process;

  // If no in flight task or in flight task is later than `next_send_time`,
  // schedule a new one. Previous in flight task will be retired.
  if (next_process_time_.IsMinusInfinity() ||
      next_process_time_ > next_send_time) {
    // Prefer low precision if allowed and not probing.
    task_queue_->PostDelayedHighPrecisionTask(
        SafeTask(
            safety_.flag(),
            [this, next_send_time]() { MaybeProcessPackets(next_send_time); }),
        time_to_next_process.RoundUpTo(TimeDelta::Millis(1)));
    next_process_time_ = next_send_time;
  }
}

void TaskQueuePacedSender::UpdateStats() {
  Stats new_stats;
  new_stats.expected_queue_time = pacing_controller_.ExpectedQueueTime();
  new_stats.first_sent_packet_time = pacing_controller_.FirstSentPacketTime();
  new_stats.oldest_packet_enqueue_time =
      pacing_controller_.OldestPacketEnqueueTime();
  new_stats.queue_size = pacing_controller_.QueueSizeData();
  OnStatsUpdated(new_stats);
}

bool TaskQueuePacedSender::ShouldPreferBestPath(
    const RtpPacketToSend& packet) const {
  if (packet.packet_type() == RtpPacketMediaType::kRetransmission ||
      packet.packet_type() == RtpPacketMediaType::kAudio) {
    return true;
  }
  if (packet.is_key_frame()) {
    return true;
  }
  if (packet.packet_type() == RtpPacketMediaType::kForwardErrorCorrection ||
      packet.packet_type() == RtpPacketMediaType::kPadding || packet.is_red()) {
    return true;
  }
  return false;
}

int TaskQueuePacedSender::SelectBestPathHint() const {
  RTC_DCHECK_RUN_ON(task_queue_);
  std::optional<MultipathQuotaHint> hint = GetMultipathQuotaHint();
  if (!hint.has_value()) {
    return RtpPacketToSend::kPrimaryPathHint;
  }
  if (hint->secondary_estimated_bps > hint->primary_estimated_bps) {
    return RtpPacketToSend::kSecondaryPathHint;
  }
  return RtpPacketToSend::kPrimaryPathHint;
}

int TaskQueuePacedSender::SelectPathHintByGccRatio() {
  RTC_DCHECK_RUN_ON(task_queue_);
  std::optional<MultipathQuotaHint> hint = GetMultipathQuotaHint();
  if (!hint.has_value()) {
    path_hint_primary_weight_acc_ = 0;
    path_hint_secondary_weight_acc_ = 0;
    recent_primary_path_load_ = 0;
    recent_secondary_path_load_ = 0;
    return RtpPacketToSend::kNoPathHint;
  }
  const int64_t primary_weight = hint->primary_estimated_bps;
  const int64_t secondary_weight = hint->secondary_estimated_bps;
  if (primary_weight <= 0 || secondary_weight <= 0) {
    path_hint_primary_weight_acc_ = 0;
    path_hint_secondary_weight_acc_ = 0;
    return SelectBestPathHint();
  }
  const int64_t total_recent = recent_primary_path_load_ + recent_secondary_path_load_;
  const int64_t total_weight = primary_weight + secondary_weight;
  if (total_recent > 0 && total_weight > 0) {
    const int64_t target_primary =
        (total_recent * primary_weight) / total_weight;
    const int64_t target_secondary = total_recent - target_primary;
    const int64_t primary_deficit = target_primary - recent_primary_path_load_;
    const int64_t secondary_deficit =
        target_secondary - recent_secondary_path_load_;
    const int64_t deficit_gap = primary_deficit - secondary_deficit;
    if (deficit_gap > kCompensationHysteresis) {
      return RtpPacketToSend::kPrimaryPathHint;
    }
    if (deficit_gap < -kCompensationHysteresis) {
      return RtpPacketToSend::kSecondaryPathHint;
    }
  }
  path_hint_primary_weight_acc_ += primary_weight;
  path_hint_secondary_weight_acc_ += secondary_weight;

  const bool prefer_primary =
      path_hint_primary_weight_acc_ >= path_hint_secondary_weight_acc_;
  if (prefer_primary) {
    path_hint_primary_weight_acc_ -= total_weight;
  } else {
    path_hint_secondary_weight_acc_ -= total_weight;
  }

  return prefer_primary ? RtpPacketToSend::kPrimaryPathHint
                        : RtpPacketToSend::kSecondaryPathHint;
}

void TaskQueuePacedSender::ObservePathHintAssignment(int path_hint) {
  RTC_DCHECK_RUN_ON(task_queue_);
  const int64_t primary_observation =
      path_hint == RtpPacketToSend::kPrimaryPathHint ? kRecentPathLoadUnit : 0;
  const int64_t secondary_observation = path_hint ==
                                                RtpPacketToSend::kSecondaryPathHint
                                            ? kRecentPathLoadUnit
                                            : 0;
  recent_primary_path_load_ =
      (recent_primary_path_load_ * kRecentPathLoadEwmaNumerator +
       primary_observation) /
      kRecentPathLoadEwmaDenominator;
  recent_secondary_path_load_ =
      (recent_secondary_path_load_ * kRecentPathLoadEwmaNumerator +
       secondary_observation) /
      kRecentPathLoadEwmaDenominator;
}

void TaskQueuePacedSender::AssignPathHintForPacket(RtpPacketToSend& packet) {
  RTC_DCHECK_RUN_ON(task_queue_);
  packet.set_path_hint(RtpPacketToSend::kNoPathHint);

  // Keep all packets from the same video frame on the same path.
  const bool frame_affinity_applicable =
      packet.packet_type() == RtpPacketMediaType::kVideo;
  if (frame_affinity_applicable) {
    std::optional<int> cached = LookupFramePathHint(packet.Ssrc(),
                                                    packet.Timestamp());
    if (cached.has_value()) {
      packet.set_path_hint(*cached);
      ObservePathHintAssignment(*cached);
      return;
    }
  }

  const int path_hint = ShouldPreferBestPath(packet)
                            ? SelectBestPathHint()
                            : SelectPathHintByGccRatio();
  packet.set_path_hint(path_hint);
  ObservePathHintAssignment(path_hint);
  if (frame_affinity_applicable &&
      (path_hint == RtpPacketToSend::kPrimaryPathHint ||
       path_hint == RtpPacketToSend::kSecondaryPathHint)) {
    RememberFramePathHint(packet.Ssrc(), packet.Timestamp(), path_hint);
  }
}

std::optional<int> TaskQueuePacedSender::LookupFramePathHint(
    uint32_t ssrc,
    uint32_t rtp_timestamp) const {
  RTC_DCHECK_RUN_ON(task_queue_);
  for (const FramePathHintEntry& entry : frame_path_hint_cache_) {
    if (entry.valid && entry.ssrc == ssrc &&
        entry.rtp_timestamp == rtp_timestamp) {
      return entry.path_hint;
    }
  }
  return std::nullopt;
}

void TaskQueuePacedSender::RememberFramePathHint(uint32_t ssrc,
                                                 uint32_t rtp_timestamp,
                                                 int path_hint) {
  RTC_DCHECK_RUN_ON(task_queue_);
  // Update existing entry if present.
  for (FramePathHintEntry& entry : frame_path_hint_cache_) {
    if (entry.valid && entry.ssrc == ssrc &&
        entry.rtp_timestamp == rtp_timestamp) {
      entry.path_hint = path_hint;
      return;
    }
  }
  // Insert at the ring-buffer write cursor (evicting the oldest slot).
  FramePathHintEntry& slot =
      frame_path_hint_cache_[frame_path_hint_cache_next_idx_];
  slot.ssrc = ssrc;
  slot.rtp_timestamp = rtp_timestamp;
  slot.path_hint = path_hint;
  slot.valid = true;
  frame_path_hint_cache_next_idx_ =
      (frame_path_hint_cache_next_idx_ + 1) % kFramePathHintCacheSize;
}

void TaskQueuePacedSender::ResetFramePathHintCache() {
  RTC_DCHECK_RUN_ON(task_queue_);
  for (FramePathHintEntry& entry : frame_path_hint_cache_) {
    entry.valid = false;
    entry.ssrc = 0;
    entry.rtp_timestamp = 0;
    entry.path_hint = -1;
  }
  frame_path_hint_cache_next_idx_ = 0;
}

TaskQueuePacedSender::Stats TaskQueuePacedSender::GetStats() const {
  RTC_DCHECK_RUN_ON(task_queue_);
  return current_stats_;
}

}  // namespace webrtc
