// tests/PacketCodecTests.cpp
//
// The codec parses bytes from untrusted peers, so most of these tests are about
// what it REFUSES. A crafted length field walking the reader off the end of a
// buffer is the single most likely memory-safety bug in this codebase, and the
// decoder's one bounds check is what prevents it.

#include "tests/TestHarness.h"

#include <string>

#include "core/protocol/PacketCodec.h"

namespace anytime::test {

using namespace anytime::protocol;

namespace {

PeerId peerFrom(Byte fill)
{
    PeerIdBytes bytes{};
    bytes.fill(fill);
    return PeerId{bytes};
}

}  // namespace

void runPacketCodecTests(Registry& r)
{
    r.section("PacketCodec — round trip");

    {
        const ByteBuffer payload{1, 2, 3, 4, 5};
        PacketToEncode toEncode;
        toEncode.type = PacketType::Announce;
        toEncode.ttl = kInitialTtl;
        toEncode.timestamp = 1700000000;
        toEncode.sender = peerFrom(0xAA);
        toEncode.payload = payload;

        ByteBuffer frame;
        r.check(PacketCodec::encode(toEncode, frame).isOk(), "encodes a broadcast packet");

        Packet decoded;
        r.check(PacketCodec::decode(frame, decoded).isOk(), "decodes it back");
        r.check(decoded.type == PacketType::Announce, "type survives");
        r.check(decoded.ttl == kInitialTtl, "ttl survives");
        r.check(decoded.timestamp == 1700000000u, "timestamp survives");
        r.check(decoded.sender == peerFrom(0xAA), "sender survives");
        r.check(decoded.isBroadcast(), "null recipient sets the broadcast flag");
        r.check(decoded.payload.size() == payload.size(), "payload length survives");
        r.check(decoded.payload.toBuffer() == payload, "payload bytes survive");
    }

    {
        const ByteBuffer payload{9, 9, 9};
        PacketToEncode toEncode;
        toEncode.type = PacketType::NoiseMessage;
        toEncode.sender = peerFrom(0x11);
        toEncode.recipient = peerFrom(0x22);
        toEncode.payload = payload;

        ByteBuffer frame;
        PacketCodec::encode(toEncode, frame);

        Packet decoded;
        r.check(PacketCodec::decode(frame, decoded).isOk(), "decodes a directed packet");
        r.check(!decoded.isBroadcast(), "directed packet is not flagged broadcast");
        r.check(decoded.recipient == peerFrom(0x22), "recipient survives");
    }

    // REGRESSION: a payload larger than 64 KB.
    //
    // This is the exact case that made images unsendable. MeshEngine encodes the
    // whole encrypted envelope as ONE packet and fragments afterwards, so a
    // 121 KB photo hit the old u16 length field and was refused with "Payload
    // exceeds the 16-bit length field" — before fragmentation or transport
    // selection ever ran. Anything that narrows this field again breaks photos.
    {
        const ByteBuffer big(150 * 1024, 0x5A);
        PacketToEncode toEncode;
        toEncode.type = PacketType::NoiseMessage;
        toEncode.sender = peerFrom(0x55);
        toEncode.recipient = peerFrom(0x66);
        toEncode.payload = big;

        ByteBuffer frame;
        r.check(PacketCodec::encode(toEncode, frame).isOk(),
                "encodes a 150 KB payload — the size a photo actually reaches");

        Packet decoded;
        r.check(PacketCodec::decode(frame, decoded).isOk(), "decodes a 150 KB payload");
        r.check(decoded.payload.size() == big.size(), "the large payload length survives");
        r.check(decoded.payload.toBuffer() == big, "every byte of the large payload survives");

        // The ceiling is the reassembly cap, and it is enforced.
        PacketToEncode tooBig = toEncode;
        const ByteBuffer overCap(kMaxReassembledBytes + 1, 0x01);
        tooBig.payload = overCap;
        ByteBuffer ignored;
        r.check(PacketCodec::encode(tooBig, ignored).isError(),
                "refuses a payload past the reassembly limit");
    }

    r.section("PacketCodec — rejects malformed input");

    {
        Packet decoded;
        const ByteBuffer empty;
        r.check(PacketCodec::decode(empty, decoded).code() == ErrorCode::MalformedPacket,
                "rejects an empty frame");

        const ByteBuffer runt{1, 2, 3};
        r.check(PacketCodec::decode(runt, decoded).code() == ErrorCode::MalformedPacket,
                "rejects a frame shorter than the header");
    }

    {
        const ByteBuffer payload{1, 2, 3};
        PacketToEncode toEncode;
        toEncode.type = PacketType::Announce;
        toEncode.sender = peerFrom(0x33);
        toEncode.payload = payload;

        ByteBuffer frame;
        PacketCodec::encode(toEncode, frame);

        // Unknown protocol version: reject rather than guess (AD-04).
        ByteBuffer wrongVersion = frame;
        wrongVersion[0] = 99;
        Packet decoded;
        r.check(PacketCodec::decode(wrongVersion, decoded).code() == ErrorCode::UnsupportedVersion,
                "rejects an unknown protocol version");

        ByteBuffer wrongType = frame;
        wrongType[1] = 0x7F;
        r.check(PacketCodec::decode(wrongType, decoded).code() == ErrorCode::MalformedPacket,
                "rejects an unknown packet type");

        // THE important one: a payload length larger than the bytes that
        // actually arrived must not be trusted.
        // All FOUR bytes of the length field — writing only the top half would
        // still be rejected, but for the wrong reason, and would stop testing
        // the boundary the moment the field widened.
        ByteBuffer lying = frame;
        lying[kFixedHeaderBytes]     = 0x00;
        lying[kFixedHeaderBytes + 1] = 0x00;
        lying[kFixedHeaderBytes + 2] = 0xFF;
        lying[kFixedHeaderBytes + 3] = 0xFF;  // claims 65535 bytes of payload
        r.check(PacketCodec::decode(lying, decoded).code() == ErrorCode::MalformedPacket,
                "rejects a payload length that exceeds the frame");

        // A length beyond anything a receiver would reassemble is refused on its
        // own terms, before the frame-size arithmetic.
        ByteBuffer absurd = frame;
        absurd[kFixedHeaderBytes]     = 0xFF;
        absurd[kFixedHeaderBytes + 1] = 0xFF;
        absurd[kFixedHeaderBytes + 2] = 0xFF;
        absurd[kFixedHeaderBytes + 3] = 0xFF;  // ~4 GB
        r.check(PacketCodec::decode(absurd, decoded).code() == ErrorCode::MalformedPacket,
                "rejects an implausible declared length outright");

        // Truncation anywhere must fail rather than read past the end.
        bool allTruncationsRejected = true;
        for (std::size_t cut = 1; cut < frame.size(); ++cut) {
            ByteBuffer truncated(frame.begin(), frame.begin() + cut);
            Packet ignored;
            if (PacketCodec::decode(truncated, ignored).isOk()) {
                allTruncationsRejected = false;
                break;
            }
        }
        r.check(allTruncationsRejected, "rejects every truncation of a valid frame");
    }

    {
        // A packet flagged as signed but with no room for the 64-byte signature.
        const ByteBuffer payload{7};
        const ByteBuffer fakeSignature(kSignatureBytes, 0xEE);

        PacketToEncode toEncode;
        toEncode.type = PacketType::Announce;
        toEncode.sender = peerFrom(0x44);
        toEncode.payload = payload;
        toEncode.signature = fakeSignature;

        ByteBuffer frame;
        PacketCodec::encode(toEncode, frame);

        Packet decoded;
        r.check(PacketCodec::decode(frame, decoded).isOk(), "decodes a signed packet");
        r.check(decoded.isSigned(), "signed flag is set");
        r.check(decoded.signature.size() == kSignatureBytes, "signature is 64 bytes");

        ByteBuffer clipped(frame.begin(), frame.end() - 10);
        r.check(PacketCodec::decode(clipped, decoded).isError(),
                "rejects a signed packet with a clipped signature");
    }

    r.section("PacketCodec — padding buckets");

    r.check(paddedSizeFor(10) == 256, "small payload pads to 256");
    r.check(paddedSizeFor(256) == 256, "exact bucket size stays put");
    r.check(paddedSizeFor(257) == 512, "just over a bucket moves up");
    r.check(paddedSizeFor(2048) == 2048, "the largest bucket is still a bucket");
    r.check(paddedSizeFor(2049) == 4096, "past the largest bucket rounds to its next multiple");
    r.check(paddedSizeFor(5000) == 6144,
            "a photo-sized payload is rounded, not sent at its exact length");
    r.check(paddedSizeFor(kMaxMediaBytes + 1000) % 2048 == 0
                && paddedSizeFor(kMaxMediaBytes + 1000) <= kMaxReassembledBytes,
            "the largest photo still fits the reassembly limit once padded");

    r.section("Padding — inside the encryption");

    {
        const ByteBuffer shortText = {'o', 'k'};
        ByteBuffer longText(200, 'x');

        const ByteBuffer a = padPlaintext(shortText);
        const ByteBuffer b = padPlaintext(longText);
        r.check(a.size() == 256 && b.size() == 256,
                "a 2-byte and a 200-byte message pad to the same size");

        ByteBuffer back;
        r.check(unpadPlaintext(a, back).isOk() && back == shortText,
                "unpadding restores the exact short message");
        r.check(unpadPlaintext(b, back).isOk() && back == longText,
                "unpadding restores the exact long message");

        // The prefix counts, so a message exactly one bucket long moves up.
        ByteBuffer exact(256, 'y');
        r.check(padPlaintext(exact).size() == 512,
                "the length prefix is counted when choosing the bucket");

        ByteBuffer empty;
        r.check(unpadPlaintext(padPlaintext(empty), back).isOk() && back.empty(),
                "an empty plaintext survives the round trip");

        ByteBuffer photo(150 * 1024, 0xAB);
        const ByteBuffer paddedPhoto = padPlaintext(photo);
        r.check(paddedPhoto.size() % 2048 == 0 && paddedPhoto.size() >= photo.size() + 4,
                "a photo pads to a multiple of 2 KB");
        r.check(unpadPlaintext(paddedPhoto, back).isOk() && back == photo,
                "and comes back byte for byte");
    }

    {
        // Hostile or corrupt input: the length prefix must never be believed
        // past the end of the buffer.
        ByteBuffer back;
        const ByteBuffer tooShort = {0x00, 0x00};
        r.check(unpadPlaintext(tooShort, back).isError(), "rejects a buffer with no length prefix");

        ByteBuffer lying = padPlaintext(ByteBuffer{'h', 'i'});
        lying[0] = 0x7F;  // claims ~2 GB
        r.check(unpadPlaintext(lying, back).isError(), "rejects a length larger than the buffer");

        ByteBuffer offByOne(8, 0);
        offByOne[3] = 5;  // 4-byte prefix + 5 declared > 8 available
        r.check(unpadPlaintext(offByOne, back).isError(), "rejects a length one past the end");
        offByOne[3] = 4;
        r.check(unpadPlaintext(offByOne, back).isOk() && back.size() == 4,
                "accepts a length that exactly fills the buffer");

        ByteBuffer randomFill = padPlaintext(ByteBuffer{'h', 'i'});
        randomFill.back() = 0x5A;
        r.check(unpadPlaintext(randomFill, back).isOk() && back == ByteBuffer({'h', 'i'}),
                "non-zero fill is tolerated, so a later build may change the filler");
    }
}

}  // namespace anytime::test
