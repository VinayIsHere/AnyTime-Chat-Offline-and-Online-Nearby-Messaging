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

/// Request bodies larger than this are rejected outright. The local API only
/// ever receives small JSON, so a large body means something is wrong.
inline constexpr std::size_t kMaxRequestBodyBytes = 64 * 1024;

}  // namespace anytime
