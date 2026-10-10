// tests/MediaEnvelopeTests.cpp
//
// The CBOR envelope codec, with the image payload as the focus.
//
// This suite exists because the codec had NO test coverage at all until media
// was added, and it is the one place where a mistake is invisible until two
// devices disagree in the field: an encoder and a decoder that are wrong in the
// same way round-trip perfectly.
//
// So these check the properties that a round-trip alone would not catch — that
// image bytes travel as a CBOR byte string rather than base64 text, that an
// oversized image is refused before it is ever encrypted and fragmented, and
// that a hostile payload is rejected on arrival rather than trusted.

#include "tests/TestHarness.h"

#include "adapters/codec/QtCborCodec.h"
#include "core/domain/Types.h"

namespace anytime::test {

namespace {

protocol::AppEnvelope sampleMediaEnvelope()
{
    protocol::ChatMediaPayload media;
    media.clientMessageId = ClientMessageId{"cmid-1"};
    media.mimeType = "image/jpeg";
    media.caption = "at the beach";
    media.bytes = ByteBuffer{0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10};  // JPEG magic
    media.sentAt = 1'700'000'000'000LL;

    protocol::AppEnvelope envelope;
    envelope.type = protocol::EnvelopeType::ChatMedia;
    envelope.media = media;
    return envelope;
}

}  // namespace

void runMediaEnvelopeTests(Registry& registry)
{
    registry.section("Media envelope (CBOR)");

    codec::QtCborCodec codec;

    // ── round trip ───────────────────────────────────────────────────────────

    const protocol::AppEnvelope original = sampleMediaEnvelope();

    ByteBuffer encoded;
    registry.check(codec.encode(original, encoded).isOk(), "media envelope encodes");
    registry.check(!encoded.empty(), "encoded media is not empty");

    protocol::AppEnvelope back;
    registry.check(codec.decode(ByteSpan{encoded}, back).isOk(), "media envelope decodes");
    registry.check(back.type == protocol::EnvelopeType::ChatMedia, "envelope type survives");
    registry.check(back.media.has_value(), "media payload is present after decode");
    registry.check(back.media->bytes == original.media->bytes,
                   "image bytes survive the round trip exactly");
    registry.check(back.media->mimeType == "image/jpeg", "mime type survives");
    registry.check(back.media->caption == "at the beach", "caption survives");
    registry.check(back.media->clientMessageId.value() == "cmid-1", "client message id survives");
    registry.check(back.media->sentAt == 1'700'000'000'000LL, "sent-at survives");

    // A 6-byte image base64'd would cost a third more plus quoting. Keeping this
    // a CBOR byte string is worth real airtime on a radio this slow.
    registry.check(encoded.size() < 120,
                   "a small image encodes compactly — byte string, not base64 text");

    // ── the sender must not be able to send an oversized image ──────────────

    protocol::AppEnvelope oversized = sampleMediaEnvelope();
    oversized.media->bytes.assign(protocol::kMaxMediaBytes + 1, 0x41);

    ByteBuffer ignored;
    registry.check(codec.encode(oversized, ignored).isError(),
                   "an oversized image is refused at encode, before encryption and fragmenting");

    protocol::AppEnvelope atLimit = sampleMediaEnvelope();
    atLimit.media->bytes.assign(protocol::kMaxMediaBytes, 0x41);
    registry.check(codec.encode(atLimit, ignored).isOk(), "an image exactly at the limit is allowed");

    // ── malformed envelopes ──────────────────────────────────────────────────

    protocol::AppEnvelope missingPayload;
    missingPayload.type = protocol::EnvelopeType::ChatMedia;
    registry.check(codec.encode(missingPayload, ignored).isError(),
                   "a media envelope with no payload is refused");

    // A peer claiming media but sending no bytes must be rejected on arrival
    // rather than stored as a zero-length image.
    protocol::AppEnvelope empty = sampleMediaEnvelope();
    empty.media->bytes.clear();
    ByteBuffer emptyEncoded;
    if (codec.encode(empty, emptyEncoded).isOk()) {
        protocol::AppEnvelope emptyBack;
        registry.check(codec.decode(ByteSpan{emptyEncoded}, emptyBack).isError(),
                       "a media envelope carrying no image is rejected on arrival");
    } else {
        registry.check(true, "a media envelope carrying no image never encodes");
    }

    // ── the other envelope types still work ─────────────────────────────────
    //
    // Adding a case to a switch is exactly where neighbouring cases get broken.

    protocol::AppEnvelope text;
    text.type = protocol::EnvelopeType::ChatMessage;
    protocol::ChatMessagePayload message;
    message.body = "hello";
    message.clientMessageId = ClientMessageId{"cmid-2"};
    message.sentAt = 42;
    text.message = message;

    ByteBuffer textEncoded;
    registry.check(codec.encode(text, textEncoded).isOk(), "text message still encodes");

    protocol::AppEnvelope textBack;
    registry.check(codec.decode(ByteSpan{textEncoded}, textBack).isOk(),
                   "text message still decodes");
    registry.check(textBack.message.has_value() && textBack.message->body == "hello",
                   "text body is unaffected by the media case");
    registry.check(!textBack.media.has_value(), "a text envelope carries no media payload");

    // ── forward compatibility (section 07) ──────────────────────────────────
    //
    // An older build must skip an envelope type it does not know rather than
    // crash. This is the rule media itself relies on to be deployable.

    ByteBuffer unknownType = textEncoded;
    bool patched = false;
    for (std::size_t i = 0; i + 1 < unknownType.size(); ++i) {
        // The type key (1) followed by ChatMessage (4); rewrite the value to an
        // envelope type no build defines.
        if (unknownType[i] == 0x01
            && unknownType[i + 1] == static_cast<Byte>(protocol::EnvelopeType::ChatMessage)) {
            unknownType[i + 1] = 99;
            patched = true;
            break;
        }
    }
    if (patched) {
        protocol::AppEnvelope unknownBack;
        registry.check(codec.decode(ByteSpan{unknownType}, unknownBack).isError(),
                       "an unknown envelope type is refused, not crashed on");
    }

    // ── garbage in ───────────────────────────────────────────────────────────

    protocol::AppEnvelope garbageBack;
    const ByteBuffer garbage{0x00, 0x01, 0x02, 0x03};
    registry.check(codec.decode(ByteSpan{garbage}, garbageBack).isError(),
                   "non-CBOR input is rejected");
    registry.check(codec.decode(ByteSpan{}, garbageBack).isError(), "empty input is rejected");
}

}  // namespace anytime::test
