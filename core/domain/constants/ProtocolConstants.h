// core/domain/constants/ProtocolConstants.h
//
// The mesh tuning knobs (docs/ARCHITECTURE.md section 07).
//
// Almost every number here comes from bitchat's published whitepaper, which is
// public domain. They represent field tuning we get for free — do not change one
// without a measurement to justify it.
//
// Our framing is deliberately NOT wire-compatible with bitchat (AD-04): own
// service UUID, own version byte, header shaped for directed one-to-one traffic
// rather than public broadcast.

#pragma once

#include <cstddef>

#include "core/domain/Primitives.h"

namespace anytime::protocol {

// ── Identity of the protocol itself ──────────────────────────────────────────

/// Bump only for breaking changes. Receivers reject unknown majors rather than
/// guessing.
inline constexpr Byte kProtocolVersion = 1;

/// Our BLE service UUID. Deliberately not bitchat's — we are a separate network.
///
/// TODO(phase-2): replace with a real random UUID before the first field build.
/// Shipping this placeholder would make every AnyTime install collide with any
/// other project that copied the same example value.
inline constexpr const char* kServiceUuid = "00000000-a17e-4a17-b17e-000000000001";

// ── Header geometry ──────────────────────────────────────────────────────────

inline constexpr std::size_t kFixedHeaderBytes    = 12;  ///< through the sender id
inline constexpr std::size_t kRecipientFieldBytes = 8;
inline constexpr std::size_t kPayloadLengthBytes  = 2;

// ── Fragmentation ────────────────────────────────────────────────────────────

/// Conservative and fixed. We never query MTU: platforms report it unreliably
/// with several centrals connected, and being wrong means silent truncation.
/// Text does not need the throughput. bitchat uses ~469; we trade it for safety.
inline constexpr std::size_t kFragmentPayloadBytes = 180;

/// [ fragmentId : 8 ][ index : u16 ][ total : u16 ]
inline constexpr std::size_t kFragmentHeaderBytes = 12;
inline constexpr std::size_t kFragmentIdBytes     = 8;

// ── Reassembly bounds — a hostile peer must not exhaust memory ───────────────

inline constexpr std::size_t kMaxConcurrentReassemblies = 128;
inline constexpr Timestamp   kReassemblyTimeoutMillis   = 30LL * 1000;
inline constexpr std::size_t kMaxReassembledBytes       = 1024 * 1024;

// ── TTL and relay  (AD-09: fields exist now, behaviour is a policy flip) ─────

inline constexpr Byte kInitialTtl      = 7;
inline constexpr Byte kDenseGraphTtl   = 5;   ///< clamp when link count >= below
inline constexpr int  kDenseGraphLinks = 6;

// ── Duplicate suppression ────────────────────────────────────────────────────
//
// Without this a relayed packet loops between neighbours until its TTL burns
// out, multiplying traffic and battery cost.

inline constexpr std::size_t kDedupCapacity  = 1000;
inline constexpr Timestamp   kDedupTtlMillis = 5LL * 60 * 1000;

// ── Announce cadence ─────────────────────────────────────────────────────────
//
// Fast while isolated so discovery feels instant; backs off once peers are
// known. The jitter matters: without it, devices that started together stay
// synchronised and collide on every broadcast.

inline constexpr Timestamp kAnnounceIsolatedMillis     = 4LL * 1000;
inline constexpr Timestamp kAnnounceConnectedMinMillis = 15LL * 1000;
inline constexpr Timestamp kAnnounceConnectedMaxMillis = 30LL * 1000;

/// Neighbour ids carried in an announce, for topology mapping.
inline constexpr std::size_t kMaxAnnouncedNeighbours = 10;

// ── Padding buckets for encrypted packets ────────────────────────────────────
//
// Hides plaintext length from a passive listener. Cleartext types (announce,
// fragment) travel at natural length, so their sizes remain observable — an
// accepted trade, since they carry no private content.

inline constexpr std::size_t kPaddingBuckets[] = {256, 512, 1024, 2048};

}  // namespace anytime::protocol
