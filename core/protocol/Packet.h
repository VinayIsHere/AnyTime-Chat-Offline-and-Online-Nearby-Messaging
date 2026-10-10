// core/protocol/Packet.h
//
// The decoded packet shape. The wire format itself and every tuning value live
// in core/domain/constants/ProtocolConstants.h; PacketType lives in Enums.h.
//
// Our own protocol identity, derived from bitchat's public-domain design but NOT
// wire-compatible with it (AD-04).
//
//  0       1       2       3       4       5       6       7
// +-------+-------+-------+-------+-------+-------+-------+-------+
// | ver   | type  | ttl   | flags |     timestamp (u32, seconds)  |
// +-------+-------+-------+-------+-------+-------+-------+-------+
// |            sender id — first 8 bytes of key fingerprint       |
// +---------------------------------------------------------------+
// |   recipient id — 8 bytes, absent when broadcast (flag bit 0)  |
// +---------------------------------------------------------------+
// | payload length (u32) | payload …                              |
// +---------------------------------------------------------------+
// |        [ Ed25519 signature — 64 bytes, ANNOUNCE only ]        |
// +---------------------------------------------------------------+

#pragma once

#include <cstdint>

#include "core/domain/Result.h"
#include "core/domain/Types.h"

namespace anytime::protocol {

namespace flags {
inline constexpr Byte kBroadcast = 0x01;  ///< recipient id absent
inline constexpr Byte kSigned    = 0x02;  ///< trailing Ed25519 signature present
inline constexpr Byte kPadded    = 0x04;  ///< plaintext was padded before encryption
}  // namespace flags

/// A decoded packet. `payload` and `signature` point INTO the caller's buffer —
/// copy them if they need to outlive the decode call.
struct Packet {
    Byte          version   = kProtocolVersion;
    PacketType    type      = PacketType::Announce;
    Byte          ttl       = kInitialTtl;
    Byte          flagBits  = 0;
    std::uint32_t timestamp = 0;   ///< seconds since epoch, for dedup keying

    PeerId   sender;
    PeerId   recipient;            ///< null when broadcast
    ByteSpan payload;
    ByteSpan signature;            ///< 64 bytes when flags::kSigned, else empty

    bool isBroadcast() const { return (flagBits & flags::kBroadcast) != 0; }
    bool isSigned()    const { return (flagBits & flags::kSigned) != 0; }
    bool isPadded()    const { return (flagBits & flags::kPadded) != 0; }
};

/// Rounds up to the next entry in kPaddingBuckets, and above the largest bucket
/// to the next multiple of it. Encrypted packets only — cleartext types travel
/// at natural length.
std::size_t paddedSizeFor(std::size_t plaintextSize) noexcept;

/// Pads a plaintext BEFORE it is encrypted:
///
///   [ real length : u32 BE ][ plaintext ][ zero fill ... ]
///
/// sized to paddedSizeFor(prefix + plaintext). The receiver strips it after
/// decrypting, trusting the prefix only because the AEAD has authenticated it.
ByteBuffer padPlaintext(ByteSpan plaintext);

/// Reverses padPlaintext. MalformedPacket if the declared length does not fit.
/// The fill is not required to be zero, so a later build may choose another
/// filler without breaking this one.
Result unpadPlaintext(ByteSpan padded, ByteBuffer& plaintext);

}  // namespace anytime::protocol
