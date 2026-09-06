// core/domain/constants/RequestConstants.h
//
// Anti-abuse limits for the request gate (docs/ARCHITECTURE.md section 08).
//
// There is no server to enforce any of this, so every check runs locally on the
// receiving device. That also means these numbers are advisory against a
// determined attacker who modifies their own client — they exist to make casual
// spam unrewarding, not to be a security boundary.

#pragma once

#include "core/domain/Primitives.h"

namespace anytime {

/// Inbound requests accepted from one peer id within the window below. Further
/// requests are dropped silently — telling a blocked sender they were blocked is
/// itself a signal worth withholding.
inline constexpr int       kMaxInboundRequestsPerPeer  = 3;
inline constexpr Timestamp kInboundRequestWindowMillis = 60LL * 60 * 1000;

/// Maximum characters in the optional greeting attached to a request. Kept small
/// because it is unsolicited content shown before any trust decision.
inline constexpr std::size_t kMaxGreetingChars = 140;

/// Username validation (AD-05). Uniqueness is deliberately NOT checked — with no
/// server there is no registry to check against.
inline constexpr std::size_t kMinUsernameChars = 3;
inline constexpr std::size_t kMaxUsernameChars = 20;

/// Stated on the welcome screen and enforced at account creation.
inline constexpr int kMinimumAgeYears = 13;

/// Maximum characters in a single chat message.
inline constexpr std::size_t kMaxMessageChars = 2000;

}  // namespace anytime
