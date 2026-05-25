/*
 *  Copyright 2026 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef EXAMPLES_PEERCONNECTION_CLIENT_APPLY_WEBRTC_STACK_FROM_FLAGS_H_
#define EXAMPLES_PEERCONNECTION_CLIENT_APPLY_WEBRTC_STACK_FROM_FLAGS_H_

// Interprets FLAGS_webrtc_send_stack and configures multipath experiment hooks
// via rtc_base. Call immediately after absl::ParseCommandLine. Returns false if
// the flag value is not recognized.
bool ApplyWebrtcStackModeFromFlags();

#endif  // EXAMPLES_PEERCONNECTION_CLIENT_APPLY_WEBRTC_STACK_FROM_FLAGS_H_
