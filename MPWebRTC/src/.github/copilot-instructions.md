# WebRTC Copilot Guide

## Architecture quick map
- `//api/` is the public surface; headers must mirror `.cc` files and keep non-API includes out of headers per `api/README.md`.
- `//pc/` hosts PeerConnection orchestration that stitches media, transport, and signaling—changes ripple into call setup and SDP handling.
- `//modules/` contains focused components (audio_processing, rtp_rtcp, codecs); each folder owns its GN targets and test binaries.
- `//rtc_base/` provides logging (`RTC_LOG`), threading (`rtc::Thread`), and ref-count helpers; alterations affect every platform build.
- `//examples/peerconnection` shows end-to-end signaling; `client/conductor.cc` wires UI callbacks to `PeerConnectionInterface` and queues work back to the UI thread via `MainWindow`.
- GN root `BUILD.gn` aggregates into `:webrtc`; enabling `rtc_build_examples`/`rtc_build_tools` adds `//examples` and `//rtc_tools` trees.

## Build & configuration
- Generate once per output dir: `gn gen out/Default --args='is_debug=true rtc_include_tests=true'`.
- Build with `autoninja -C out/Default webrtc`; append explicit targets (e.g. `peerconnection_client`, `rtc_unittests`) to tighten rebuilds.
- Feature toggles live in `webrtc.gni` (`rtc_use_h264`, `rtc_use_pipewire`, `rtc_build_examples`); keep arg combinations consistent with CI configs.
- Inspect current args via `gn args out/Default --list` before flipping new switches or reproducing bot failures.

## Testing workflow
- Unit suites sit beside their code: `//:rtc_unittests`, `pc:peerconnection_unittests`, `modules/audio_processing:audio_processing_tests`, etc.
- Build the suite (`autoninja -C out/Default rtc_unittests`) then run `out/Default/rtc_unittests --gtest_filter=...`.
- `BUILD.gn` ties optional data bundles (e.g. `rtc_unittests_bundle_data`) to tests—add resources there rather than hard-coding paths.
- Example smoke checks: `autoninja -C out/Default peerconnection_client` followed by `out/Default/peerconnection_client --server=<ip> --port=8888`.

## Coding patterns to preserve
- Logging and checks use `RTC_LOG`, `RTC_DCHECK`; avoid raw `std::cout` or Chromium-only macros.
- Ownership is via `rtc::scoped_refptr` and `webrtc::scoped_refptr`; release references explicitly only when transferring ownership (see `Conductor::UIThreadCallback`).
- Signaling runs on dedicated threads created by `webrtc::Thread::CreateWithSocketServer`; heavy work should be posted to worker/task queues to prevent deadlocks.
- Public API edits must follow the deprecation flow in `PRESUBMIT.py`/`native-api.md`—announce changes, mark deprecated, and schedule removals.

## Presubmit and GN hygiene
- PRESUBMIT forbids `<iostream>` in headers and raw `FRIEND_TEST`; use `FRIEND_TEST_ALL_PREFIXES` from `testsupport/gtest_prod_util.h`.
- GN files may not reach above their directory (`../` paths trigger presubmit failures); introduce new targets near the code instead.
- Keep cpplint clean—respect the exception lists in `PRESUBMIT.py` and prefer structuring new code under existing module patterns.
- Format BUILD files with `gn format <file>`; C++ style follows `g3doc/style-guide.md`.

## Helpful references
- `docs/native-code/development/README.md` covers fetch, `gn gen`, and `autoninja` flows for every platform.
- `docs/native-code/development/contributing.md` documents `git cl upload`, reviewer expectations, and trybots.
- `native-api.md` lists API-safe directories—confirm ownership before moving headers or changing visibility.
