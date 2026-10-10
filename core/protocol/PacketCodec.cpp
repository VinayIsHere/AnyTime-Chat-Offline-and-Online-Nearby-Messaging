// core/protocol/PacketCodec.cpp
//
// Every input here is attacker-controlled: a hostile peer can send any bytes it
// likes. Every length is therefore validated against the actual buffer size
// before it is used, and no read is performed on the strength of a declared
// length alone.

#include "core/protocol/PacketCodec.h"

#include <cstring>
#include <iterator>

namespace anytime::protocol {

namespace {

void appendU32(ByteBuffer& out, std::uint32_t v)
{
    out.push_back(static_cast<Byte>((v >> 24) & 0xFF));
    out.push_back(static_cast<Byte>((v >> 16) & 0xFF));
    out.push_back(static_cast<Byte>((v >> 8) & 0xFF));
    out.push_back(static_cast<Byte>(v & 0xFF));
}

std::uint32_t readU32(ByteSpan s, std::size_t offset)
{
    return (static_cast<std::uint32_t>(s[offset]) << 24)
           | (static_cast<std::uint32_t>(s[offset + 1]) << 16)
           | (static_cast<std::uint32_t>(s[offset + 2]) << 8)
           | static_cast<std::uint32_t>(s[offset + 3]);
}

bool isKnownType(Byte raw)
{
    switch (static_cast<PacketType>(raw)) {
        case PacketType::Announce:
        case PacketType::NoiseHandshake:
        case PacketType::NoiseMessage:
        case PacketType::Fragment:
        case PacketType::Ack:
            return true;
    }
    return false;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

std::size_t paddedSizeFor(std::size_t plaintextSize) noexcept
{
    for (const std::size_t bucket : kPaddingBuckets) {
        if (plaintextSize <= bucket) return bucket;
    }
    // Larger than the biggest bucket — a photo. Round up to a multiple of it
    // rather than sending at natural length, which would publish the exact
    // byte count of every image.
    constexpr std::size_t step = kPaddingBuckets[std::size(kPaddingBuckets) - 1];
    return ((plaintextSize + step - 1) / step) * step;
}

ByteBuffer padPlaintext(ByteSpan plaintext)
{
    const std::size_t total = paddedSizeFor(kPaddingLengthPrefixBytes + plaintext.size());

    ByteBuffer out;
    out.reserve(total);
    appendU32(out, static_cast<std::uint32_t>(plaintext.size()));
    out.insert(out.end(), plaintext.begin(), plaintext.end());
    out.resize(total, 0);  // zeros are safe: they are encrypted with the rest
    return out;
}

Result unpadPlaintext(ByteSpan padded, ByteBuffer& plaintext)
{
    if (padded.size() < kPaddingLengthPrefixBytes) {
        return Result::error(ErrorCode::MalformedPacket, "Padded plaintext has no length.");
    }
    const std::size_t length = readU32(padded, 0);
    if (length > padded.size() - kPaddingLengthPrefixBytes) {
        return Result::error(ErrorCode::MalformedPacket,
                             "Declared plaintext length exceeds the padded buffer.");
    }
    const auto begin = padded.begin() + kPaddingLengthPrefixBytes;
    plaintext.assign(begin, begin + static_cast<std::ptrdiff_t>(length));
    return Result::ok();
}

std::size_t PacketCodec::encodedSize(const PacketToEncode& p) noexcept
{
    std::size_t size = kFixedHeaderBytes;
    if (!p.recipient.isNull()) size += kRecipientFieldBytes;
    size += kPayloadLengthBytes + p.payload.size();
    size += p.signature.size();
    return size;
}

Result PacketCodec::encode(const PacketToEncode& p, ByteBuffer& out)
{
    // Bounded by what a receiver will ever reassemble, not by the width of the
    // length field. Encoding something larger would only produce a packet the
    // far side is obliged to discard.
    if (p.payload.size() > kMaxReassembledBytes) {
        return Result::error(ErrorCode::InvalidArgument,
                             "Payload exceeds the reassembly limit.");
    }
    if (!p.signature.empty() && p.signature.size() != kSignatureBytes) {
        return Result::error(ErrorCode::InvalidArgument, "Signature must be 64 bytes.");
    }

    Byte flagBits = 0;
    if (p.recipient.isNull()) flagBits |= flags::kBroadcast;
    if (!p.signature.empty()) flagBits |= flags::kSigned;
    if (p.pad) flagBits |= flags::kPadded;

    out.reserve(out.size() + encodedSize(p));

    out.push_back(kProtocolVersion);
    out.push_back(static_cast<Byte>(p.type));
    out.push_back(p.ttl);
    out.push_back(flagBits);
    appendU32(out, p.timestamp);

    const PeerIdBytes& sender = p.sender.bytes();
    out.insert(out.end(), sender.begin(), sender.end());

    if (!p.recipient.isNull()) {
        const PeerIdBytes& recipient = p.recipient.bytes();
        out.insert(out.end(), recipient.begin(), recipient.end());
    }

    appendU32(out, static_cast<std::uint32_t>(p.payload.size()));
    out.insert(out.end(), p.payload.begin(), p.payload.end());

    if (!p.signature.empty()) out.insert(out.end(), p.signature.begin(), p.signature.end());

    return Result::ok();
}

Result PacketCodec::decode(ByteSpan frame, Packet& packet)
{
    if (frame.size() < kFixedHeaderBytes) {
        return Result::error(ErrorCode::MalformedPacket, "Frame shorter than the fixed header.");
    }

    packet.version = frame[0];
    if (packet.version != kProtocolVersion) {
        // Reject rather than guess. AD-04: we own this byte, so an unknown
        // version means a peer we cannot safely interpret.
        return Result::error(ErrorCode::UnsupportedVersion, "Unknown protocol version.");
    }

    if (!isKnownType(frame[1])) {
        return Result::error(ErrorCode::MalformedPacket, "Unknown packet type.");
    }
    packet.type = static_cast<PacketType>(frame[1]);
    packet.ttl = frame[2];
    packet.flagBits = frame[3];
    packet.timestamp = readU32(frame, 4);

    std::size_t offset = 8;

    PeerIdBytes senderBytes{};
    std::memcpy(senderBytes.data(), frame.data() + offset, kPeerIdBytes);
    packet.sender = PeerId{senderBytes};
    offset += kPeerIdBytes;

    if (!packet.isBroadcast()) {
        if (frame.size() < offset + kRecipientFieldBytes) {
            return Result::error(ErrorCode::MalformedPacket, "Truncated recipient id.");
        }
        PeerIdBytes recipientBytes{};
        std::memcpy(recipientBytes.data(), frame.data() + offset, kPeerIdBytes);
        packet.recipient = PeerId{recipientBytes};
        offset += kRecipientFieldBytes;
    } else {
        packet.recipient = PeerId{};
    }

    if (frame.size() < offset + kPayloadLengthBytes) {
        return Result::error(ErrorCode::MalformedPacket, "Missing payload length.");
    }
    const std::size_t payloadLength = readU32(frame, offset);
    offset += kPayloadLengthBytes;

    // Refuse an absurd declared length up front. The bounds check below would
    // catch it anyway, but a hostile peer should not get to name a number this
    // code then carries around in arithmetic.
    if (payloadLength > kMaxReassembledBytes) {
        return Result::error(ErrorCode::MalformedPacket, "Declared payload length is implausible.");
    }

    const std::size_t signatureLength = packet.isSigned() ? kSignatureBytes : 0;

    // The declared payload length plus any signature must fit exactly within
    // what actually arrived. This single check is what stops a crafted length
    // from walking the reader off the end of the buffer.
    if (frame.size() < offset + payloadLength + signatureLength) {
        return Result::error(ErrorCode::MalformedPacket,
                             "Declared payload length exceeds the frame.");
    }

    packet.payload = frame.subspan(offset, payloadLength);
    offset += payloadLength;

    packet.signature = signatureLength > 0 ? frame.subspan(offset, signatureLength) : ByteSpan{};

    return Result::ok();
}

ByteSpan PacketCodec::signedRegion(ByteSpan frame)
{
    // Everything from the version byte through the end of the payload, i.e. the
    // whole frame minus a trailing signature. Signer and verifier must derive
    // this identically or every announce fails to verify.
    Packet packet;
    if (decode(frame, packet).isError()) return {};

    const std::size_t signatureLength = packet.isSigned() ? kSignatureBytes : 0;
    if (frame.size() < signatureLength) return {};

    return frame.subspan(0, frame.size() - signatureLength);
}

}  // namespace anytime::protocol
