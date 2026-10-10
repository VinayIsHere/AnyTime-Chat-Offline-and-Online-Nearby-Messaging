// core/services/ChatService.cpp
//
// Logs here carry ids, peer ids and lengths — never a message body.

#include "core/services/ChatService.h"

#include "core/log/Log.h"
#include "core/ports/IClock.h"
#include "core/ports/IEventSink.h"
#include "core/services/DeliveryService.h"
#include "core/services/MeshEngine.h"

namespace anytime {

ChatService::ChatService(IMessageRepository& messages, IContactRepository& contacts,
                         DeliveryService& delivery, MeshEngine& mesh, IEventSink& events,
                         IClock& clock)
    : messages_(messages),
      contacts_(contacts),
      delivery_(delivery),
      mesh_(mesh),
      events_(events),
      clock_(clock)
{
}

Result ChatService::refuseIfBlocked(const PeerId& peerId)
{
    // Their messages to us are already dropped on arrival. Letting ours go out
    // would keep feeding someone the user chose to cut off, and would leave
    // bubbles queued forever for a conversation that has ended.
    if (!contacts_.isBlocked(peerId)) return Result::ok();
    AT_LOG_INFO("chat", "send to blocked contact {} refused", peerId.toHex());
    return Result::error(ErrorCode::PermissionDenied, "You blocked this person.");
}

Outcome<std::vector<Chat>> ChatService::list()
{
    return messages_.listChats();
}

ChatSummary ChatService::summarise(const Chat& chat)
{
    ChatSummary summary;
    summary.id            = chat.id;
    summary.peerId        = chat.peerId;
    summary.lastMessageAt = chat.lastMessageAt;
    summary.unreadCount   = chat.unreadCount;

    // The pinned contact, not the live peer: a chat must still read correctly
    // when the other person is out of range, and must not be renamed by someone
    // re-announcing under a different nickname (section 08).
    auto contact = contacts_.findContact(chat.peerId);
    if (contact.isOk() && contact.value().has_value()) {
        summary.displayName = contact.value()->displayName;
        summary.blocked     = contact.value()->isBlocked();
    }

    // One row is enough — page() returns newest first.
    auto page = messages_.page(chat.id, Cursor{}, 1);
    if (page.isOk() && !page.value().messages.empty()) {
        const Message& latest = page.value().messages.front();

        // A photo's body IS its caption, and a caption is usually empty — so
        // reading the body alone left a chat whose last message was a picture
        // showing nothing at all in the list. Name the kind instead.
        //
        // The literal lives here rather than in the UI because this layer
        // already returns display-ready strings (relativeTime gives "4m",
        // handleFor gives "@vinay·4f2a"). If the app ever gains translations,
        // all three move together.
        summary.lastMessage =
            (latest.isMedia() && latest.body.empty()) ? std::string("Photo") : latest.body;
    }

    return summary;
}

Outcome<std::vector<ChatSummary>> ChatService::summaries()
{
    auto chats = messages_.listChats();
    if (chats.isError()) return chats.result();

    std::vector<ChatSummary> out;
    out.reserve(chats.value().size());
    for (const Chat& chat : chats.value()) out.push_back(summarise(chat));
    return out;
}

Outcome<ChatSummary> ChatService::summaryFor(const ChatId& chatId)
{
    auto chat = messages_.findChat(chatId);
    if (chat.isError()) return chat.result();
    if (!chat.value().has_value()) {
        return Outcome<ChatSummary>::error(ErrorCode::NotFound, "No such chat.");
    }
    return summarise(*chat.value());
}

Outcome<MessagePage> ChatService::history(const ChatId& chatId, const Cursor& cursor, int limit)
{
    if (limit <= 0) limit = kDefaultHistoryPageSize;
    if (limit > kMaxHistoryPageSize) limit = kMaxHistoryPageSize;
    return messages_.page(chatId, cursor, limit);
}

Outcome<Message> ChatService::send(const ChatId& chatId, const std::string& body,
                                   const ClientMessageId& clientMessageId)
{
    if (body.empty()) {
        return Outcome<Message>::error(ErrorCode::InvalidArgument, "Message is empty.");
    }
    if (body.size() > kMaxMessageChars) {
        return Outcome<Message>::error(ErrorCode::InvalidArgument, "Message is too long.");
    }

    auto chat = messages_.findChat(chatId);
    if (chat.isError()) return chat.result();
    if (!chat.value().has_value()) {
        return Outcome<Message>::error(ErrorCode::NotFound, "No such chat.");
    }
    if (const Result r = refuseIfBlocked(chat.value()->peerId); r.isError()) return r;

    // Idempotency: a retried POST with the same client id must not duplicate the
    // message. The UI generates that id before the first attempt precisely so a
    // dropped response cannot produce two bubbles.
    auto duplicate = messages_.findByClientId(clientMessageId);
    if (duplicate.isOk() && duplicate.value().has_value()) {
        AT_LOG_DEBUG("chat", "resend of client message {} returned the stored copy",
                     clientMessageId.value());
        return *duplicate.value();
    }

    Message message;
    message.chatId = chatId;
    message.clientMessageId = clientMessageId;
    message.direction = Direction::Outgoing;
    message.body = body;
    message.status = DeliveryStatus::Queued;
    message.createdAt = clock_.now();

    auto stored = messages_.append(message);
    if (stored.isError()) {
        AT_LOG_ERROR("chat", "could not store outgoing message in chat {}: {}", chatId.value(),
                     stored.result().message());
        return stored.result();
    }
    message.id = stored.value();

    AT_LOG_DEBUG("chat", "message {} ({} bytes) queued in chat {}", message.id.value(),
                 body.size(), chatId.value());

    protocol::AppEnvelope envelope;
    envelope.type = protocol::EnvelopeType::ChatMessage;
    protocol::ChatMessagePayload payload;
    payload.body = body;
    payload.clientMessageId = clientMessageId;
    payload.sentAt = message.createdAt;
    envelope.message = payload;

    // Hand off and return immediately. The UI renders an optimistic bubble at
    // Queued, and every later transition arrives over SSE (section 09).
    delivery_.enqueue(message.id, chat.value()->peerId, envelope, PayloadClass::Text);

    return message;
}

namespace {

/// Derived from the client message id rather than generated, so a resend of the
/// same image overwrites one blob instead of accumulating copies — the sender
/// retries whenever an ack is lost, which is normal rather than exceptional.
std::string mediaIdFor(const ClientMessageId& clientMessageId)
{
    return "media_" + clientMessageId.value();
}

}  // namespace

Outcome<Message> ChatService::sendMedia(const ChatId& chatId, const std::string& mimeType,
                                        const std::string& caption, ByteSpan bytes,
                                        const ClientMessageId& clientMessageId)
{
    if (bytes.empty()) {
        return Outcome<Message>::error(ErrorCode::InvalidArgument, "No image was supplied.");
    }
    if (bytes.size() > protocol::kMaxMediaBytes) {
        // The UI downscales before upload; reaching here means it did not.
        return Outcome<Message>::error(ErrorCode::InvalidArgument, "That image is too large.");
    }

    auto chat = messages_.findChat(chatId);
    if (chat.isError()) return chat.result();
    if (!chat.value().has_value()) {
        return Outcome<Message>::error(ErrorCode::NotFound, "No such chat.");
    }
    if (const Result r = refuseIfBlocked(chat.value()->peerId); r.isError()) return r;

    // Same idempotency rule as send(): a retried POST must not produce a second
    // bubble, and for an image it must not store the bytes twice either.
    auto duplicate = messages_.findByClientId(clientMessageId);
    if (duplicate.isOk() && duplicate.value().has_value()) {
        AT_LOG_DEBUG("chat", "resend of client message {} returned the stored copy",
                     clientMessageId.value());
        return *duplicate.value();
    }

    const std::string mediaId = mediaIdFor(clientMessageId);

    // Blob first. A message row pointing at an image that failed to store would
    // render as a permanently broken picture with no way to recover it.
    if (const Result r = messages_.putMedia(mediaId, bytes); r.isError()) {
        AT_LOG_ERROR("chat", "could not store outgoing image for chat {}: {}", chatId.value(),
                     r.message());
        return r;
    }

    Message message;
    message.chatId = chatId;
    message.clientMessageId = clientMessageId;
    message.direction = Direction::Outgoing;
    message.body = caption.substr(0, protocol::kMaxCaptionChars);
    message.status = DeliveryStatus::Queued;
    message.createdAt = clock_.now();
    message.mediaId = mediaId;
    message.mediaMimeType = mimeType;
    message.mediaBytes = bytes.size();

    auto stored = messages_.append(message);
    if (stored.isError()) {
        AT_LOG_ERROR("chat", "could not store outgoing image message in chat {}: {}",
                     chatId.value(), stored.result().message());
        return stored.result();
    }
    message.id = stored.value();

    AT_LOG_INFO("chat", "image {} ({} bytes) queued in chat {}", message.id.value(), bytes.size(),
                chatId.value());

    protocol::AppEnvelope envelope;
    envelope.type = protocol::EnvelopeType::ChatMedia;
    protocol::ChatMediaPayload payload;
    payload.clientMessageId = clientMessageId;
    payload.mimeType = mimeType;
    payload.caption = message.body;
    payload.bytes.assign(bytes.begin(), bytes.end());
    payload.sentAt = message.createdAt;
    envelope.media = payload;

    // Bulk, not Text. THIS is the classification that lets RoutingPolicy pick a
    // high-throughput transport for a photo; without it every image routed over
    // whatever carried the chat, which on Android means BLE and minutes.
    delivery_.enqueue(message.id, chat.value()->peerId, envelope, PayloadClass::Bulk);

    return message;
}

Outcome<ByteBuffer> ChatService::media(const std::string& mediaId)
{
    return messages_.getMedia(mediaId);
}

void ChatService::onMediaReceived(const PeerId& peerId, const protocol::ChatMediaPayload& payload)
{
    // The request gate, enforced here as well as at request time: a peer could
    // complete a handshake and start pushing images without ever asking.
    auto contact = contacts_.findContact(peerId);
    if (contact.isError() || !contact.value().has_value()) {
        AT_LOG_WARN("chat", "image from non-contact {} dropped", peerId.toHex());
        return;
    }
    if (contact.value()->isBlocked()) {
        AT_LOG_DEBUG("chat", "image from blocked contact {} dropped", peerId.toHex());
        return;
    }

    if (payload.bytes.empty() || payload.bytes.size() > protocol::kMaxMediaBytes) {
        AT_LOG_WARN("chat", "image from {} rejected: {} bytes", peerId.toHex(),
                    payload.bytes.size());
        return;
    }

    auto chat = messages_.findChatByPeer(peerId);
    if (chat.isError() || !chat.value().has_value()) {
        AT_LOG_WARN("chat", "contact {} has no chat; image dropped", peerId.toHex());
        return;
    }

    // A resend after a lost ack is normal, not an attack.
    auto duplicate = messages_.findByClientId(payload.clientMessageId);
    if (duplicate.isOk() && duplicate.value().has_value()) {
        AT_LOG_DEBUG("chat", "duplicate image {} from {}; re-acking",
                     payload.clientMessageId.value(), peerId.toHex());
        mesh_.sendAck(peerId, payload.clientMessageId);
        return;
    }

    const std::string mediaId = mediaIdFor(payload.clientMessageId);

    if (const Result r = messages_.putMedia(mediaId, ByteSpan{payload.bytes}); r.isError()) {
        // No ack: the sender retries, which is right when we failed to keep it.
        AT_LOG_ERROR("chat", "could not store image from {}: {}", peerId.toHex(), r.message());
        return;
    }

    Message message;
    message.chatId = chat.value()->id;
    message.clientMessageId = payload.clientMessageId;
    message.direction = Direction::Incoming;
    message.body = payload.caption.substr(0, protocol::kMaxCaptionChars);
    message.status = DeliveryStatus::Delivered;
    message.createdAt = clock_.now();
    message.mediaId = mediaId;
    message.mediaMimeType = payload.mimeType;
    message.mediaBytes = payload.bytes.size();

    auto stored = messages_.append(message);
    if (stored.isError()) {
        AT_LOG_ERROR("chat", "could not store image message from {}: {}", peerId.toHex(),
                     stored.result().message());
        return;
    }
    message.id = stored.value();

    messages_.setUnreadCount(chat.value()->id, chat.value()->unreadCount + 1);

    AT_LOG_INFO("chat", "image {} ({} bytes) received from {}", message.id.value(),
                payload.bytes.size(), peerId.toHex());

    mesh_.sendAck(peerId, payload.clientMessageId);
    events_.onMessageReceived(message);
}

Result ChatService::retry(const MessageId& messageId)
{
    auto message = messages_.findMessage(messageId);
    if (message.isError()) return message.result();
    if (!message.value().has_value()) return Result::error(ErrorCode::NotFound, "No such message.");

    auto chat = messages_.findChat(message.value()->chatId);
    if (chat.isError()) return chat.result();
    if (!chat.value().has_value()) return Result::error(ErrorCode::NotFound, "No such chat.");
    if (const Result r = refuseIfBlocked(chat.value()->peerId); r.isError()) return r;

    return delivery_.retry(messageId);
}

Result ChatService::markRead(const ChatId& chatId, const MessageId& messageId)
{
    auto chat = messages_.findChat(chatId);
    if (chat.isError()) return chat.result();
    if (!chat.value().has_value()) return Result::error(ErrorCode::NotFound, "No such chat.");

    auto message = messages_.findMessage(messageId);
    if (message.isError()) return message.result();
    if (!message.value().has_value()) {
        return Result::error(ErrorCode::NotFound, "No such message.");
    }

    if (const Result r = messages_.setStatus(messageId, DeliveryStatus::Read); r.isError()) {
        AT_LOG_ERROR("chat", "could not mark message {} read: {}", messageId.value(), r.message());
        return r;
    }
    if (const Result r = messages_.setUnreadCount(chatId, 0); r.isError()) return r;

    // Only tell the sender about messages they sent us. A read receipt for our
    // own outbound message would be nonsense.
    if (message.value()->direction == Direction::Incoming) {
        protocol::AppEnvelope envelope;
        envelope.type = protocol::EnvelopeType::ChatRead;
        protocol::ChatReadPayload payload;
        payload.clientMessageId = message.value()->clientMessageId;
        envelope.read = payload;

        mesh_.sendEnvelope(chat.value()->peerId, envelope, PayloadClass::Text);
    }
    return Result::ok();
}

// ── driven by MeshEngine ─────────────────────────────────────────────────────

void ChatService::onMessageReceived(const PeerId& peerId,
                                    const protocol::ChatMessagePayload& payload)
{
    // The request gate is enforced HERE too, not only at request time. A peer
    // could complete a handshake and start sending chat.message envelopes
    // without ever asking — being a contact is what earns delivery.
    auto contact = contacts_.findContact(peerId);
    if (contact.isError() || !contact.value().has_value()) {
        AT_LOG_WARN("chat", "message from non-contact {} dropped", peerId.toHex());
        return;
    }
    if (contact.value()->isBlocked()) {
        AT_LOG_DEBUG("chat", "message from blocked contact {} dropped", peerId.toHex());
        return;
    }

    auto chat = messages_.findChatByPeer(peerId);
    if (chat.isError() || !chat.value().has_value()) {
        AT_LOG_WARN("chat", "contact {} has no chat; message dropped", peerId.toHex());
        return;
    }

    // Replays and retransmissions arrive with the same client id; the sender
    // retries whenever an ack is lost, so this is a normal case, not an attack.
    auto duplicate = messages_.findByClientId(payload.clientMessageId);
    if (duplicate.isOk() && duplicate.value().has_value()) {
        AT_LOG_DEBUG("chat", "duplicate message {} from {}; re-acking",
                     payload.clientMessageId.value(), peerId.toHex());
        mesh_.sendAck(peerId, payload.clientMessageId);  // their ack was lost
        return;
    }

    Message message;
    message.chatId = chat.value()->id;
    message.clientMessageId = payload.clientMessageId;
    message.direction = Direction::Incoming;
    message.body = payload.body.substr(0, kMaxMessageChars);
    message.status = DeliveryStatus::Delivered;
    message.createdAt = clock_.now();

    auto stored = messages_.append(message);
    if (stored.isError()) {
        // No ack: the sender keeps retrying, which is right when we failed to keep it.
        AT_LOG_ERROR("chat", "could not store message from {}: {}", peerId.toHex(),
                     stored.result().message());
        return;
    }
    message.id = stored.value();

    messages_.setUnreadCount(chat.value()->id, chat.value()->unreadCount + 1);

    AT_LOG_DEBUG("chat", "message {} ({} bytes) received from {}", message.id.value(),
                 message.body.size(), peerId.toHex());

    mesh_.sendAck(peerId, payload.clientMessageId);
    events_.onMessageReceived(message);
}

void ChatService::onReadReceipt(const PeerId& peerId, const ClientMessageId& clientMessageId)
{
    auto contact = contacts_.findContact(peerId);
    if (contact.isError() || !contact.value().has_value()) return;

    auto message = messages_.findByClientId(clientMessageId);
    if (message.isError() || !message.value().has_value()) return;

    // Only our own outbound messages can be marked read by someone else.
    if (message.value()->direction != Direction::Outgoing) {
        AT_LOG_WARN("chat", "{} sent a read receipt for a message it did not receive",
                    peerId.toHex());
        return;
    }

    AT_LOG_DEBUG("chat", "message {} read by {}", message.value()->id.value(), peerId.toHex());
    messages_.setStatus(message.value()->id, DeliveryStatus::Read);
    events_.onMessageStatusChanged(message.value()->id, DeliveryStatus::Read);
}

}  // namespace anytime
