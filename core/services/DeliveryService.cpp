// core/services/DeliveryService.cpp
//
// The outbox and the retry ladder — what makes the app usable when someone
// drifts in and out of range (docs/ARCHITECTURE.md section 09).

#include "core/services/DeliveryService.h"

#include "core/log/Log.h"
#include "core/ports/IClock.h"
#include "core/ports/IEventSink.h"
#include "core/ports/IRepositories.h"
#include "core/services/DiscoveryService.h"
#include "core/services/MeshEngine.h"

namespace anytime {

DeliveryService::DeliveryService(MeshEngine& mesh, DiscoveryService& discovery, IOutbox& outbox,
                                 IMessageRepository& messages, protocol::ICborCodec& cbor,
                                 IEventSink& events, IClock& clock)
    : mesh_(mesh),
      discovery_(discovery),
      outbox_(outbox),
      messages_(messages),
      cbor_(cbor),
      events_(events),
      clock_(clock)
{
}

DeliveryService::~DeliveryService() { stop(); }

void DeliveryService::start()
{
    if (running_) return;
    running_ = true;
    tickTimer_ = clock_.scheduleRepeating(kDeliveryTickMillis, [this] { tick(); });
}

void DeliveryService::stop()
{
    if (!running_) return;
    running_ = false;
    if (tickTimer_ != kInvalidTimer) {
        clock_.cancel(tickTimer_);
        tickTimer_ = kInvalidTimer;
    }
}

Timestamp DeliveryService::backoffFor(int attempt)
{
    const int count = sizeof(kRetryBackoffMillis) / sizeof(kRetryBackoffMillis[0]);
    if (attempt < 0) attempt = 0;
    if (attempt >= count) return kRetryBackoffMillis[count - 1];
    return kRetryBackoffMillis[attempt];
}

Result DeliveryService::enqueue(const MessageId& messageId, const PeerId& peerId,
                                const protocol::AppEnvelope& envelope, PayloadClass payloadClass)
{
    const Timestamp now = clock_.now();

    OutboxEntry entry;
    entry.messageId = messageId;
    entry.peerId = peerId;
    entry.attempts = 0;
    entry.nextAttemptAt = now;
    entry.expiresAt = now + kOutboxTtlMillis;
    entry.payloadClass = payloadClass;

    // The encoded envelope rides IN the outbox row, so a retry does not have to
    // reconstruct it and — the part that used to be missing — it survives the
    // app being killed. Storage encrypts this at rest alongside the message body.
    if (const Result r = cbor_.encode(envelope, entry.payload); r.isError()) {
        AT_LOG_ERROR("delivery", "could not encode the envelope for message {}: {}",
                     messageId.value(), r.message());
        return r;
    }

    if (const Result r = outbox_.enqueue(entry); r.isError()) {
        AT_LOG_ERROR("delivery", "could not queue message {}: {}", messageId.value(), r.message());
        return r;
    }

    AT_LOG_DEBUG("delivery", "message {} queued for {}", messageId.value(), peerId.toHex());

    // Try immediately: in the common case the peer is right there and the
    // message lands before the UI has finished animating the bubble.
    attemptSend(entry);
    return Result::ok();
}

void DeliveryService::attemptSend(const OutboxEntry& entry)
{
    // The envelope comes back out of the stored row rather than a RAM map, which
    // is what lets a message queued before a restart still go out afterwards.
    protocol::AppEnvelope envelope;
    if (entry.payload.empty() || cbor_.decode(ByteSpan{entry.payload}, envelope).isError()) {
        // Genuinely unrecoverable: retrying forever against bytes we cannot
        // decode would keep the message queued for a day and then fail anyway.
        AT_LOG_WARN("delivery", "message {} has no decodable envelope; dropping it from the outbox",
                    entry.messageId.value());
        outbox_.remove(entry.messageId);
        setStatus(entry.messageId, DeliveryStatus::Failed);
        return;
    }

    // The stored class, not a hard-coded one. This is what lets a photo reach a
    // bulk transport while a line of text stays on the cheap radio.
    const Result sent = mesh_.sendEnvelope(entry.peerId, envelope, entry.payloadClass);
    const Timestamp retryAt = clock_.now() + backoffFor(entry.attempts);

    if (sent.isOk()) {
        AT_LOG_DEBUG("delivery", "message {} sent to {} (attempt {})", entry.messageId.value(),
                     entry.peerId.toHex(), entry.attempts + 1);
        setStatus(entry.messageId, DeliveryStatus::Sent);
        // Stays in the outbox until an ACK arrives: "sent" only means it left
        // this device, and on a mesh that is a long way from "delivered".
        outbox_.recordAttempt(entry.messageId, retryAt);
        return;
    }

    // Failure is expected and cheap — the peer is out of range, or the session is
    // still handshaking. Back off and let the tick retry.
    AT_LOG_DEBUG("delivery", "message {} not sent (attempt {}): {}", entry.messageId.value(),
                 entry.attempts + 1, sent.message());
    outbox_.recordAttempt(entry.messageId, retryAt);
}

void DeliveryService::tick()
{
    const Timestamp now = clock_.now();

    auto peers = outbox_.peersWithPending();
    if (peers.isOk()) {
        for (const PeerId& peerId : peers.value()) drainPeer(peerId);
    }

    // Anything past 8 attempts or 24 hours gives up, and the UI offers a retry.
    auto expired = outbox_.reapExpired(now);
    if (expired.isOk()) {
        for (const OutboxEntry& entry : expired.value()) {
            AT_LOG_WARN("delivery", "message {} to {} gave up after {} attempts",
                        entry.messageId.value(), entry.peerId.toHex(), entry.attempts);
            setStatus(entry.messageId, DeliveryStatus::Failed);
        }
    }
}

void DeliveryService::drainPeer(const PeerId& peerId)
{
    auto due = outbox_.dueFor(peerId, clock_.now());
    if (due.isError()) return;

    // In order: a chat that arrives shuffled reads as nonsense.
    for (const OutboxEntry& entry : due.value()) attemptSend(entry);
}

void DeliveryService::onPeerReachable(const PeerId& peerId)
{
    drainPeer(peerId);
}

void DeliveryService::onSessionEstablished(const PeerId& peerId)
{
    // Everything queued while the handshake was in flight can go now.
    drainPeer(peerId);
}

void DeliveryService::onAckReceived(const PeerId& peerId, const ClientMessageId& clientMessageId)
{
    auto message = messages_.findByClientId(clientMessageId);
    if (message.isError() || !message.value().has_value()) {
        AT_LOG_DEBUG("delivery", "ack from {} for an unknown message", peerId.toHex());
        return;
    }

    const MessageId messageId = message.value()->id;

    outbox_.remove(messageId);

    AT_LOG_DEBUG("delivery", "message {} delivered to {}", messageId.value(), peerId.toHex());

    // Never walk a Read message back to Delivered: a read receipt can overtake
    // the ack it implies, and regressing the tick marks would be visible.
    if (message.value()->status != DeliveryStatus::Read) {
        setStatus(messageId, DeliveryStatus::Delivered);
    }
}

Result DeliveryService::retry(const MessageId& messageId)
{
    auto message = messages_.findMessage(messageId);
    if (message.isError()) return message.result();
    if (!message.value().has_value()) return Result::error(ErrorCode::NotFound, "No such message.");

    // Only a message of ours that has given up. Anything else is either not
    // ours to send — an inbound message "retried" would go back to its own
    // author — or already on its way: a Queued row is in the outbox and a
    // second enqueue would race the first, while a Delivered one would be
    // dragged back to Queued and shown to the user as unsent.
    if (!message.value()->isMine()) {
        return Result::error(ErrorCode::InvalidArgument, "Only your own messages can be resent.");
    }
    if (message.value()->status != DeliveryStatus::Failed) {
        return Result::error(ErrorCode::PreconditionFailed, "That message has not failed.");
    }

    auto chat = messages_.findChat(message.value()->chatId);
    if (chat.isError() || !chat.value().has_value()) {
        return Result::error(ErrorCode::NotFound, "No such chat.");
    }

    AT_LOG_INFO("delivery", "manual retry of message {}", messageId.value());

    protocol::AppEnvelope envelope;
    PayloadClass payloadClass = PayloadClass::Text;

    if (message.value()->isMedia()) {
        // Rebuilding a photo as a ChatMessage would have resent it as an empty
        // line of text — the caption — and silently lost the image. The bytes
        // are still in the media store, so the envelope is rebuilt properly.
        auto bytes = messages_.getMedia(message.value()->mediaId);
        if (bytes.isError()) {
            AT_LOG_ERROR("delivery", "cannot retry message {}: its image is gone",
                         messageId.value());
            return Result::error(ErrorCode::NotFound, "That image is no longer stored.");
        }

        envelope.type = protocol::EnvelopeType::ChatMedia;
        protocol::ChatMediaPayload media;
        media.clientMessageId = message.value()->clientMessageId;
        media.mimeType = message.value()->mediaMimeType;
        media.caption = message.value()->body;
        media.bytes = bytes.value();
        media.sentAt = message.value()->createdAt;
        envelope.media = media;
        payloadClass = PayloadClass::Bulk;
    } else {
        envelope.type = protocol::EnvelopeType::ChatMessage;
        protocol::ChatMessagePayload payload;
        payload.body = message.value()->body;
        payload.clientMessageId = message.value()->clientMessageId;
        payload.sentAt = message.value()->createdAt;
        envelope.message = payload;
    }

    setStatus(messageId, DeliveryStatus::Queued);
    return enqueue(messageId, chat.value()->peerId, envelope, payloadClass);
}

void DeliveryService::setStatus(const MessageId& messageId, DeliveryStatus status)
{
    if (const Result r = messages_.setStatus(messageId, status); r.isError()) {
        AT_LOG_ERROR("delivery", "could not set message {} to {}: {}", messageId.value(),
                     toString(status), r.message());
        return;
    }
    events_.onMessageStatusChanged(messageId, status);
}

}  // namespace anytime
