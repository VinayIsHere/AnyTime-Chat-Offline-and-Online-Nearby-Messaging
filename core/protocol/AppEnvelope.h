// core/protocol/AppEnvelope.h
//
// Application payloads, CBOR-encoded, carried inside NOISE_MESSAGE packets (§07).
//
// CBOR because: it is compact on a byte-starved link, and unknown map keys are
// skippable — so an older build receives a newer message type without crashing.
// That property matters a great deal once phase 5 starts adding fields.
//
// NOTE: the encoder is declared here but CBOR itself comes from QCborStreamWriter
// in the adapter layer, because core/ may not include Qt. The core defines the
// SHAPE; adapters/codec supplies the bytes. See ICborCodec below.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/domain/Result.h"
#include "core/domain/Types.h"

namespace anytime::protocol {

struct ChatRequestPayload {
    std::string nickname;
    std::string greeting;
};

struct ChatAcceptPayload {
    std::string nickname;
};

struct ChatMessagePayload {
    ClientMessageId clientMessageId;
    std::string     body;
    Timestamp       sentAt = kNoTimestamp;
};

struct ChatReadPayload {
    ClientMessageId clientMessageId;
};

/// A decoded envelope. Exactly one payload member is populated, per `type`.
struct AppEnvelope {
    EnvelopeType type = EnvelopeType::ChatMessage;

    std::optional<ChatRequestPayload> request;
    std::optional<ChatAcceptPayload>  accept;
    std::optional<ChatMessagePayload> message;
    std::optional<ChatReadPayload>    read;
};

/// CBOR serialisation, implemented in adapters/codec over QCborStreamWriter.
///
/// This port exists solely to keep Qt out of core/ (§04). If we later drop Qt
/// for a standalone CBOR library, only the adapter changes.
class ICborCodec {
public:
    virtual ~ICborCodec() = default;

    virtual Result encode(const AppEnvelope&, ByteBuffer& out) = 0;
    virtual Result decode(ByteSpan, AppEnvelope& out)          = 0;
};

}  // namespace anytime::protocol
