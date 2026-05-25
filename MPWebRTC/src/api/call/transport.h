/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef API_CALL_TRANSPORT_H_
#define API_CALL_TRANSPORT_H_

#include <stdint.h>

#include "api/array_view.h"

namespace webrtc {

// TODO(holmer): Look into unifying this with the PacketOptions in
// asyncpacketsocket.h.
struct PacketOptions {
  PacketOptions();
  PacketOptions(const PacketOptions&);
  ~PacketOptions();

  // Negative ids are invalid and should be interpreted
  // as packet_id not being set.
  int64_t packet_id = -1;
  // Whether this is an audio or video packet, excluding retransmissions.
  // Defaults to `false` which is the more common case.
  bool is_media = false;
  bool included_in_feedback = false;
  bool included_in_allocation = false;
  bool send_as_ect1 = false;
  // Whether this packet can be part of a packet batch at lower levels.
  bool batchable = false;
  // Whether this packet is the first packet of a sender batch.
  bool first_packet_in_batch = false;
  // Whether this packet is the last of a batch.
  bool last_packet_in_batch = false;
  // Priority classes:
  // 0: retransmission > 1: audio > 2: key frame > 3: redundancy > 4: other.
  int packet_priority = 4;
  // Pacing probe cluster id, -1 means not a probe packet.
  int probe_cluster_id = -1;
  // Sender batch duration in milliseconds.
  int batch_duration_ms = 0;
  // Path hint classes:
  // -1: unset, 0: primary path, 1: secondary path.
  int path_hint = -1;
};

class Transport {
 public:
  virtual bool SendRtp(ArrayView<const uint8_t> packet,
                       const PacketOptions& options) = 0;
  virtual bool SendRtcp(ArrayView<const uint8_t> packet,
                        const PacketOptions& options) = 0;

 protected:
  virtual ~Transport() = default;
};

}  // namespace webrtc

#endif  // API_CALL_TRANSPORT_H_
