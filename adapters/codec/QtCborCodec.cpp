// adapters/codec/QtCborCodec.cpp

#include "adapters/codec/QtCborCodec.h"

#include <QCborMap>
#include <QCborValue>

namespace anytime::codec {

namespace {

// Integer keys rather than strings: on a 180-byte frame budget, "clientMessageId"
// as a map key would cost more than the value it labels. Never renumber.
enum Key : int {
    kType = 1,
    kNickname = 2,
    kGreeting = 3,
    kBody = 4,
    kClientMessageId = 5,
    kSentAt = 6,
};

QString str(const QCborMap& map, Key key)
{
    return map.value(static_cast<qint64>(key)).toString();
}

}  // namespace

Result QtCborCodec::encode(const protocol::AppEnvelope& envelope, ByteBuffer& out)
{
    QCborMap map;
    map.insert(static_cast<qint64>(kType), static_cast<int>(envelope.type));

    switch (envelope.type) {
        case protocol::EnvelopeType::ChatRequest:
            if (!envelope.request) {
                return Result::error(ErrorCode::InvalidArgument, "Missing request payload.");
            }
            map.insert(static_cast<qint64>(kNickname),
                       QString::fromStdString(envelope.request->nickname));
            map.insert(static_cast<qint64>(kGreeting),
                       QString::fromStdString(envelope.request->greeting));
            break;

        case protocol::EnvelopeType::ChatAccept:
            if (!envelope.accept) {
                return Result::error(ErrorCode::InvalidArgument, "Missing accept payload.");
            }
            map.insert(static_cast<qint64>(kNickname),
                       QString::fromStdString(envelope.accept->nickname));
            break;

        case protocol::EnvelopeType::ChatDecline:
            break;

        case protocol::EnvelopeType::ChatMessage:
            if (!envelope.message) {
                return Result::error(ErrorCode::InvalidArgument, "Missing message payload.");
            }
            map.insert(static_cast<qint64>(kBody),
                       QString::fromStdString(envelope.message->body));
            map.insert(static_cast<qint64>(kClientMessageId),
                       QString::fromStdString(envelope.message->clientMessageId.value()));
            map.insert(static_cast<qint64>(kSentAt),
                       static_cast<qint64>(envelope.message->sentAt));
            break;

        case protocol::EnvelopeType::ChatRead:
            if (!envelope.read) {
                return Result::error(ErrorCode::InvalidArgument, "Missing read payload.");
            }
            map.insert(static_cast<qint64>(kClientMessageId),
                       QString::fromStdString(envelope.read->clientMessageId.value()));
            break;
    }

    const QByteArray encoded = map.toCborValue().toCbor();
    out.assign(reinterpret_cast<const Byte*>(encoded.constData()),
               reinterpret_cast<const Byte*>(encoded.constData() + encoded.size()));
    return Result::ok();
}

Result QtCborCodec::decode(ByteSpan bytes, protocol::AppEnvelope& out)
{
    const QCborValue value = QCborValue::fromCbor(
        QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size())));

    if (!value.isMap()) {
        return Result::error(ErrorCode::MalformedPacket, "Envelope is not a CBOR map.");
    }
    const QCborMap map = value.toMap();

    const int rawType = map.value(static_cast<qint64>(kType)).toInteger(-1);
    switch (static_cast<protocol::EnvelopeType>(rawType)) {
        case protocol::EnvelopeType::ChatRequest: {
            out.type = protocol::EnvelopeType::ChatRequest;
            protocol::ChatRequestPayload payload;
            payload.nickname = str(map, kNickname).toStdString();
            payload.greeting = str(map, kGreeting).toStdString();
            out.request = payload;
            break;
        }
        case protocol::EnvelopeType::ChatAccept: {
            out.type = protocol::EnvelopeType::ChatAccept;
            protocol::ChatAcceptPayload payload;
            payload.nickname = str(map, kNickname).toStdString();
            out.accept = payload;
            break;
        }
        case protocol::EnvelopeType::ChatDecline:
            out.type = protocol::EnvelopeType::ChatDecline;
            break;

        case protocol::EnvelopeType::ChatMessage: {
            out.type = protocol::EnvelopeType::ChatMessage;
            protocol::ChatMessagePayload payload;
            payload.body = str(map, kBody).toStdString();
            payload.clientMessageId = ClientMessageId{str(map, kClientMessageId).toStdString()};
            payload.sentAt = map.value(static_cast<qint64>(kSentAt)).toInteger(0);
            out.message = payload;
            break;
        }
        case protocol::EnvelopeType::ChatRead: {
            out.type = protocol::EnvelopeType::ChatRead;
            protocol::ChatReadPayload payload;
            payload.clientMessageId = ClientMessageId{str(map, kClientMessageId).toStdString()};
            out.read = payload;
            break;
        }
        default:
            // Forward compatibility (section 07): an older build must not crash
            // on a message type a newer peer introduced.
            return Result::error(ErrorCode::UnsupportedVersion, "Unknown envelope type.");
    }
    return Result::ok();
}

}  // namespace anytime::codec
