/*
 *  Copyright 2026 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "examples/peerconnection/client/apply_webrtc_stack_from_flags.h"

#include <cstdio>
#include <string>

#include "absl/flags/declare.h"
#include "absl/flags/flag.h"
#include "rtc_base/network_route_probe_bitrate_tracker.h"

ABSL_DECLARE_FLAG(std::string, webrtc_send_stack);

namespace {

void ToLowerAsciiInPlace(std::string* s) {
  for (char& c : *s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
}

}  // namespace

bool ApplyWebrtcStackModeFromFlags() {
  std::string mode = absl::GetFlag(FLAGS_webrtc_send_stack);
  ToLowerAsciiInPlace(&mode);
  if (mode == "native" || mode == "stock") {
    webrtc::SetWebrtcMultipathSendMode(webrtc::WebrtcMultipathSendMode::kNative);
    printf("peerconnection_client: webrtc_send_stack=%s (native)\n",
           mode.c_str());
    return true;
  }
  if (mode == "extended" || mode == "experiment" || mode == "multipath") {
    webrtc::SetWebrtcMultipathSendMode(
        webrtc::WebrtcMultipathSendMode::kExtended);
    printf("peerconnection_client: webrtc_send_stack=%s (extended)\n",
           mode.c_str());
    return true;
  }
  if (mode == "round_robin" || mode == "roundrobin" || mode == "rr") {
    webrtc::SetWebrtcMultipathSendMode(
        webrtc::WebrtcMultipathSendMode::kRoundRobin);
    printf("peerconnection_client: webrtc_send_stack=%s (round_robin)\n",
           mode.c_str());
    return true;
  }
  if (mode == "single_path_pacer" || mode == "singlepath_pacer" ||
      mode == "prepacer_priority") {
    webrtc::SetWebrtcMultipathSendMode(
        webrtc::WebrtcMultipathSendMode::kSinglePathPacerPriority);
    printf("peerconnection_client: webrtc_send_stack=%s "
           "(single_path_pacer_priority)\n",
           mode.c_str());
    return true;
  }
  fprintf(stderr,
          "Invalid --webrtc_send_stack=\"%s\": use native|stock, "
          "extended|experiment|multipath, round_robin|roundrobin|rr, or "
          "single_path_pacer|singlepath_pacer|prepacer_priority.\n",
          absl::GetFlag(FLAGS_webrtc_send_stack).c_str());
  return false;
}
