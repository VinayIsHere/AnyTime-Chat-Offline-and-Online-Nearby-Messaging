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
///
/// 2: the payload length field widened from u16 to u32 (see kPayloadLengthBytes),
/// which moves every byte after it. A v1 build reading a v2 frame would take the
/// top half of the length as the whole length and misparse the rest as garbage,
/// so the bump is what turns a silent corruption into the clean
/// UnsupportedVersion rejection decode() already implements. Both handsets must
/// be reinstalled together; there was never a released v1 to support.
///
/// 3: encrypted payloads are now genuinely padded (see kPaddingBuckets). The
/// kPadded flag was set on every v2 NoiseMessage while nothing was padded, so
/// a v2 build would read a v3 plaintext's length prefix as CBOR and drop every
/// message. The bump turns that into a clean rejection. Reinstall all handsets
/// together, as for v2.
inline constexpr Byte kProtocolVersion = 3;

/// Our BLE service UUID. Deliberately not bitchat's — we are a separate network.
///
/// Randomly generated (v4), not patterned. The earlier placeholder was a
/// hand-typed value full of zeros, which defeats the point of a 122-bit random
/// namespace: any other project that copied the same example would have appeared
/// in our scan results, and we in theirs, wasting the handful of concurrent GATT
/// slots a chipset allows on devices that can never complete a handshake.
///
/// CHANGING THIS IS A BREAKING CHANGE. Devices advertise it and filter on it, so
/// a build with a different UUID is invisible to every already-installed build —
/// a silent partition with no error message. The matching RX/TX characteristic
/// UUIDs live in BleRadio.java and were regenerated at the same time.
inline constexpr const char* kServiceUuid = "41a42c6d-af86-4960-bd0c-b8bb84dd202c";

// ── Header geometry ──────────────────────────────────────────────────────────

/// version(1) + type(1) + ttl(1) + flags(1) + timestamp(4) + senderId(8).
/// Counted wrong (12) until a codec test caught it: encodedSize() then
/// under-reported every frame by 4 bytes, which would have let a packet near the
/// frame budget be built and then rejected by the transport.
inline constexpr std::size_t kFixedHeaderBytes    = 16;
inline constexpr std::size_t kRecipientFieldBytes = 8;

/// Payload length field. FOUR bytes, not two.
///
/// It was u16, and that quietly made images impossible: MeshEngine encrypts the
/// whole envelope, encodes it as ONE packet, and only then fragments — so a
/// 121 KB photo was refused by the encoder with "Payload exceeds the 16-bit
/// length field" before fragmentation or transport selection ever ran. The
/// Fragmenter existed to solve exactly that and never got the chance.
///
/// The extra two bytes cost nothing that matters: they are paid once per packet,
/// while the packet they describe is immediately split into ~468-byte fragments
/// anyway. kMaxReassembledBytes remains the real ceiling.
inline constexpr std::size_t kPayloadLengthBytes  = 4;

// ── Fragmentation ────────────────────────────────────────────────────────────

/// Fixed, and still never derived from a negotiated MTU: platforms report MTU
/// unreliably with several centrals connected, and sizing frames from it means
/// silent truncation when the report is wrong.
///
/// 480 rather than the original 180. The two numbers are independent concerns
/// that were previously conflated: this is how big a PROTOCOL frame may be,
/// while MTU decides how many BLE writes carrying it cost. Android negotiates a
/// 517-byte MTU, and the Bluetooth spec caps one attribute write at 512 octets,
/// so a 480-byte frame plus BleRadio's 2-byte length prefix lands inside a
/// SINGLE write — where 180 bytes wasted most of every write and a full frame
/// still cost nine of them at the old 20-byte default.
///
/// Lowering it is always safe and raising it is bounded by that 512-octet write:
/// keep this under 510. If MTU negotiation fails and the link stays at 23 bytes,
/// nothing breaks — BleRadio simply chunks the frame into more writes.
///
/// bitchat uses ~469, so this sits in the same family rather than inventing a
/// number. It is NOT a wire-compatibility constant: Reassembler is size-agnostic,
/// so a peer sending different-sized fragments still reassembles correctly.
inline constexpr std::size_t kFragmentPayloadBytes = 480;

/// [ fragmentId : 8 ][ index : u16 ][ total : u16 ]
inline constexpr std::size_t kFragmentHeaderBytes = 12;
inline constexpr std::size_t kFragmentIdBytes     = 8;

// ── Reassembly bounds — a hostile peer must not exhaust memory ───────────────

inline constexpr std::size_t kMaxConcurrentReassemblies = 128;
inline constexpr Timestamp   kReassemblyTimeoutMillis   = 30LL * 1000;
inline constexpr std::size_t kMaxReassembledBytes       = 1024 * 1024;

// ── Session handshake ────────────────────────────────────────────────────────

/// A Noise handshake still in flight after this long is abandoned so it can
/// start over. XX is three messages and completes in well under a second on a
/// working link, so reaching this means a handshake packet was lost — and
/// without a timeout, one lost packet would stall that conversation for good.
inline constexpr Timestamp kHandshakeTimeoutMillis = 10LL * 1000;

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

// ── Media ────────────────────────────────────────────────────────────────────

/// Largest image accepted for sending, AFTER the UI has downscaled and
/// re-encoded it. The UI is responsible for getting under this — a modern phone
/// camera produces 3-8 MB originals, which must never reach the protocol.
///
/// THE BINDING LIMIT IS BLE'S SEND QUEUE, not the protocol.
///
/// MeshEngine hands every fragment to the transport in one synchronous burst,
/// and BleRadio.MAX_QUEUED_CHUNKS refuses a frame once a link has 512 chunks
/// still waiting. A 480-byte frame plus its 2-byte length prefix is one chunk at
/// the negotiated MTU, and GATT drains one write at a time — far slower than the
/// loop pushing them — so the backlog is what a large image actually runs into:
/// past roughly 512 x 468 = 240 KB the link starts refusing frames outright.
///
/// 192 KB keeps a real margin under that (~410 fragments) and is untroubled
/// over Nearby, whose 32 KB frames carry the same image in six.
///
/// Raising this means fixing the burst first — pacing sends against the
/// transport, or raising the queue — not just changing the number here.
///
/// Be aware what it costs on BLE regardless: 192 KB at roughly 1-2 KB/s is
/// minutes per photo. That is the radio's ceiling, and precisely the case
/// Nearby Connections exists to fix (docs/NEARBY-CONNECTIONS.md).
inline constexpr std::size_t kMaxMediaBytes = 192 * 1024;

/// Longest caption attached to an image.
inline constexpr std::size_t kMaxCaptionChars = 500;

// ── Padding buckets for encrypted packets ────────────────────────────────────
//
// Hides plaintext length from a passive listener. The padding goes INSIDE the
// encryption (protocol::padPlaintext), as the Noise specification recommends:
// the filler is then ciphertext like the rest, so zeros are safe and no random
// source is needed in the core. Padding appended after encryption would have
// needed cryptographic randomness, or a run of filler would mark exactly where
// the real message ended.
//
// Above the largest bucket, sizes round up to a multiple of it. That keeps a
// photo's exact byte count private, though not its rough size: a 150 KB image
// is still visibly bigger than a line of text, because its fragment count is.
// Hiding that would mean padding every photo to kMaxMediaBytes, which costs
// minutes of BLE airtime per photo — not a trade v1 makes.
//
// Cleartext types (announce, ack, handshake) travel at natural length, so their
// sizes remain observable — an accepted trade, since they carry no private
// content and are near fixed-size anyway.

inline constexpr std::size_t kPaddingBuckets[] = {256, 512, 1024, 2048};

/// The real plaintext length, carried in front of it inside the padded buffer.
inline constexpr std::size_t kPaddingLengthPrefixBytes = 4;

}  // namespace anytime::protocol
