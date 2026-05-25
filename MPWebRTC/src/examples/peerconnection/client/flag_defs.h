/*
 *  Copyright 2012 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef EXAMPLES_PEERCONNECTION_CLIENT_FLAG_DEFS_H_
#define EXAMPLES_PEERCONNECTION_CLIENT_FLAG_DEFS_H_

#include <string>

#include "absl/flags/flag.h"

extern const uint16_t kDefaultServerPort;  // From defaults.[h|cc]

// Define flags for the peerconnect_client testing tool, in a separate
// header file so that they can be shared across the different main.cc's
// for each platform.

ABSL_FLAG(bool,
          autoconnect,
          false,
          "Connect to the server without user "
          "intervention.");
ABSL_FLAG(std::string, server, "localhost", "The server to connect to.");
ABSL_FLAG(int,
          port,
          kDefaultServerPort,
          "The port on which the server is listening.");
ABSL_FLAG(
    bool,
    autocall,
    false,
    "Call the first available other client on "
    "the server without user intervention.  Note: this flag should only be set "
    "to true on one of the two clients.");

ABSL_FLAG(
    std::string,
    force_fieldtrials,
    "",
    "Field trials control experimental features. This flag specifies the field "
    "trials in effect. E.g. running with "
    "--force_fieldtrials=WebRTC-FooFeature/Enabled/ "
    "will assign the group Enabled to field trial WebRTC-FooFeature. Multiple "
    "trials are separated by \"/\"");

// Selects this tree's multipath-related send/ICE hooks vs. stock WebRTC
// behavior for the same code paths. Parsed in ApplyWebrtcStackModeFromFlags().
ABSL_FLAG(std::string,
          webrtc_send_stack,
          "extended",
          "Send stack: \"native\", \"extended\" (dual ICE + quota + pacer hints), "
          "\"round_robin\" / \"rr\", or \"single_path_pacer\" (one ICE path but "
          "keep pre-pacer priority sort + virtual path-hint window).");

ABSL_FLAG(bool,
          record_test_metrics,
          false,
          "Record receive-side test metrics (throughput/fps/freeze/QP) to CSV. "
          "Enable on only one peer in symmetric calls.");
ABSL_FLAG(std::string,
          metrics_output_path,
          "peerconnection_metrics.csv",
          "CSV output path for test metrics.");
ABSL_FLAG(int,
          metrics_duration_seconds,
          180,
          "Metrics collection duration in seconds.");
ABSL_FLAG(int,
          metrics_interval_seconds,
          2,
          "Metrics collection interval in seconds.");

ABSL_FLAG(int,
          video_jitter_buffer_min_delay_ms,
          500,
          "Minimum receive-side video playout delay in ms. "
          "A positive value lets the jitter buffer wait for out-of-order "
          "frames and treats this waiting time as normal playout latency.");

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_FLAG_DEFS_H_
