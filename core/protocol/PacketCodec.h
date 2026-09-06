// core/protocol/PacketCodec.h
//
// Encode and decode the wire format in Packet.h. Pure functions over bytes —
// no state, no I/O, no allocation beyond the output buffer. This is the most
// heavily unit-tested file in the project: every malformed input a hostile peer
// could send must produce an error, never a crash or an out-of-bounds read.

#pragma once

#include "core/domain/Result.h"
#include "core/domain/Types.h"
#include "core/protocol/Packet.h"

namespace anytime::protocol {

/// Fields the caller supplies; the codec fills in version and geometry.
struct PacketToEncode {
    PacketType    type;
    Byte          ttl       = kInitialTtl;
    std::uint32_t timestamp = 0;
    PeerId        sender;
    PeerId        recipient;         ///< leave null for broadcast
    ByteSpan      payload;
    ByteSpan      signature;         ///< optional; sets flags::kSigned when present
    bool          pad       = false; ///< set for encrypted types
};

class PacketCodec {
public:
    /// Appends the encoded packet to `out`. Fails only on oversized payloads.
    static Result encode(const PacketToEncode&, ByteBuffer& out);

    /// Decodes in place. `packet.payload` and `.signature` are views INTO
    /// `frame` — they are invalidated when `frame` dies.
    ///
    /// Rejects: short buffers, unknown versions, truncated payloads, payload
    /// lengths that disagree with the frame, and signed packets with no room
    /// for the trailing signature.
    static Result decode(ByteSpan frame, Packet& packet);

    /// The exact bytes an ANNOUNCE signature covers: everything from the version
    /// byte through the end of the payload, excluding the signature itself.
    /// Both signer and verifier must derive it the same way.
    static ByteSpan signedRegion(ByteSpan frame);

    /// Total encoded size for a payload, so callers can check it against
    /// TransportCapabilities::maxFramePayload before fragmenting.
    static std::size_t encodedSize(const PacketToEncode&) noexcept;
};

}  // namespace anytime::protocol
