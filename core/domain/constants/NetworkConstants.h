// core/domain/constants/NetworkConstants.h
//
// Local API server parameters (docs/ARCHITECTURE.md section 10, AD-08).
//
// The server is loopback-only and exists solely so the WebView can reach the
// C++ core over ordinary HTTP. It is never exposed off-device.

#pragma once

#include <cstddef>

#include "core/domain/Primitives.h"

namespace anytime {

/// Bytes of randomness in the secret path prefix, regenerated every launch.
///
/// This is the real gate, not the ephemeral port. Android's loopback interface
/// is shared by every app on the device, so anything installed can probe
/// 127.0.0.1 — but it cannot guess 256 bits. Every route lives under
/// /{token}/..., and anything else returns 404 with no hint a server exists.
///
/// A path prefix beats a bearer token because it is race-free: C++ generates it,
/// builds the URL and hands it to the WebView, so there is no window in which
/// the page has loaded but not yet received its credential.
inline constexpr std::size_t kApiTokenBytes = 32;

/// 0 asks the OS for an ephemeral port. Never hard-code one.
inline constexpr int kDefaultApiPort = 0;

/// Keeps an idle SSE stream from being closed by an intermediary.
inline constexpr Timestamp kSseHeartbeatMillis = 20LL * 1000;

/// Default page size for chat history.
inline constexpr int kDefaultHistoryPageSize = 50;
inline constexpr int kMaxHistoryPageSize     = 200;

/// Request bodies larger than this are rejected outright.
///
/// Raised from 64 KB to carry an image upload. The WebView POSTs a downscaled
/// photo to the core as a request body, and at 64 KB that upload failed before
/// any radio was involved — the cap, not Bluetooth, was the first ceiling a
/// photo hit.
///
/// Kept only a little above kMaxMediaBytes (192 KB) rather than made generous:
/// the whole body is buffered in memory by the parser before dispatch, so this
/// value is also the most memory a single unauthenticated request can make the
/// process allocate. Every other endpoint still receives small JSON.
///
/// It tracks kMaxMediaBytes down as well as up. This was 768 KB when the media
/// cap was 512 KB; leaving it there once the cap fell to 192 KB would have kept
/// the number while quietly abandoning the reason for it.
inline constexpr std::size_t kMaxRequestBodyBytes = 256 * 1024;

}  // namespace anytime
