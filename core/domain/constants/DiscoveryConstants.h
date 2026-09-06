// core/domain/constants/DiscoveryConstants.h
//
// Peer liveness and signal banding (docs/ARCHITECTURE.md section 11).
//
// LIVENESS COMES FROM ANNOUNCES, NOT LINK EVENTS. BLE connection callbacks are
// unreliable on every platform — iOS only infers that a central connected once
// it tries to read or write. So presence is defined entirely by the window
// below, and link up/down is a resource concern rather than a correctness one.

#pragma once

#include "core/domain/Primitives.h"

namespace anytime {

/// A peer counts as reachable while a signature-verified ANNOUNCE sits inside
/// this window. At the connected cadence (15-30 s jittered) that is roughly
/// three missed announces before a peer disappears from the Nearby list.
inline constexpr Timestamp kPeerReachableWindow = 60LL * 1000;

/// How often DiscoveryService sweeps for peers that have aged out.
inline constexpr Timestamp kPeerSweepIntervalMillis = 5LL * 1000;

/// Peers not seen for this long are dropped from storage entirely.
inline constexpr Timestamp kPeerPruneAfterMillis = 7LL * 24 * 60 * 60 * 1000;

// ── RSSI banding ─────────────────────────────────────────────────────────────
//
// Raw dBm is far too noisy to show a user; it swings 10+ dB from a hand moving.
// Three bands is all the UI needs, and hysteresis on top of these thresholds
// stops a peer flickering between bands while sitting still.

inline constexpr int kRssiVeryStrongDbm = -55;  ///< same room, close
inline constexpr int kRssiStrongDbm     = -75;  ///< same room
                                                ///< below that: Weak

/// Applied when moving between bands, so a peer at a boundary does not flicker.
inline constexpr int kRssiHysteresisDbm = 4;

}  // namespace anytime
