// core/domain/constants/DeliveryConstants.h
//
// Outbox and retry tuning (docs/ARCHITECTURE.md section 09).
//
// These are what make the app usable when someone drifts in and out of range.
// The ladder starts short because peers are usually momentarily out of range
// rather than gone, and stretches out so a genuinely absent peer costs little.

#pragma once

#include <cstddef>

#include "core/domain/Primitives.h"

namespace anytime {

/// Attempts before a message is marked Failed and surfaced with a Retry action.
inline constexpr int kMaxSendAttempts = 8;

/// Backoff before each retry, indexed by attempt count. Past the end, the
/// message fails.
///
/// The length is asserted equal to kMaxSendAttempts in core/HeaderCheck.cpp —
/// the two live together here so they cannot drift, but the assertion catches it
/// if someone edits one and not the other.
inline constexpr Timestamp kRetryBackoffMillis[] = {
    2'000,       //  2 s   — peer probably just moved behind a wall
    5'000,       //  5 s
    15'000,      // 15 s
    60'000,      //  1 min
    300'000,     //  5 min
    900'000,     // 15 min
    3'600'000,   //  1 h
    10'800'000   //  3 h   — last try before giving up
};

/// A queued message is abandoned after this long regardless of attempts.
inline constexpr Timestamp kOutboxTtlMillis = 24LL * 60 * 60 * 1000;

/// Cap per peer, so one unreachable contact cannot fill the database.
inline constexpr int kMaxOutboxPerPeer = 100;

/// How long the delivery timer waits between drain sweeps.
inline constexpr Timestamp kDeliveryTickMillis = 2LL * 1000;

}  // namespace anytime
