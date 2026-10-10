// core/services/ChatService.h
//
// Chat list, history paging and message send. Backs the /chats routes.
//
// send() NEVER BLOCKS ON THE RADIO. It appends the message as Queued, hands it
// to DeliveryService and returns immediately, so the UI can render an optimistic
// bubble. Every later transition arrives as a message.status SSE event (§09).

#pragma once

#include <string>
#include <vector>

#include "core/domain/Entities.h"
#include "core/domain/Result.h"
#include "core/ports/IRepositories.h"
#include "core/protocol/AppEnvelope.h"

namespace anytime {

class IEventSink;
class IClock;
class DeliveryService;
class MeshEngine;

class ChatService {
public:
    ChatService(IMessageRepository&, IContactRepository&, DeliveryService&,
                MeshEngine&, IEventSink&, IClock&);

    /// GET /{t}/api/chats — most recent activity first.
    Outcome<std::vector<Chat>> list();

    /// GET /{t}/api/chats — the rows the chat list actually renders, each Chat
    /// joined with its pinned contact name and last message. Prefer this over
    /// list(): a bare Chat has no name on it, so serialising one produces a row
    /// the UI draws blank.
    Outcome<std::vector<ChatSummary>> summaries();

    /// GET /{t}/api/chats/:id — one summary, so a chat screen opened straight
    /// from a link or a just-accepted request can name its peer without
    /// fetching the whole list.
    Outcome<ChatSummary> summaryFor(const ChatId&);

    /// GET /{t}/api/chats/:id/messages?before=<cursor>
    Outcome<MessagePage> history(const ChatId&, const Cursor&, int limit = 50);

    /// POST /{t}/api/chats/:id/messages — returns the row at status Queued.
    Outcome<Message> send(const ChatId&, const std::string& body,
                          const ClientMessageId&);

    /// POST /{t}/api/chats/:id/media — an image.
    ///
    /// The bytes must already be within kMaxMediaBytes: downscaling is the UI's
    /// job, because it is the only layer that can re-encode a photo without
    /// decoding it here. `caption` becomes the message body.
    ///
    /// Stores the blob BEFORE the message row, so a row can never reference an
    /// image that is not there. Returns at status Queued exactly like send().
    Outcome<Message> sendMedia(const ChatId&, const std::string& mimeType,
                               const std::string& caption, ByteSpan bytes,
                               const ClientMessageId&);

    /// GET /{t}/api/media/:id — the decrypted image bytes.
    Outcome<ByteBuffer> media(const std::string& mediaId);

    /// Marks messages read up to and including `messageId`, clears the unread
    /// count and sends a chat.read receipt.
    Result markRead(const ChatId&, const MessageId&);

    /// POST /{t}/api/messages/:id/retry — sends a Failed message of ours again.
    /// Refused while the contact is blocked, for the same reason send() is; the
    /// rest of the rules live in DeliveryService::retry().
    Result retry(const MessageId&);

    // ── driven by MeshEngine ─────────────────────────────────────────────────

    /// Stores an inbound message and emits message.received. Drops messages from
    /// peers with no accepted contact — the request gate is enforced here too,
    /// not only at request time.
    void onMessageReceived(const PeerId&, const protocol::ChatMessagePayload&);

    /// Stores an inbound image and emits message.received. Enforces the same
    /// contact gate as onMessageReceived — being a contact is what earns
    /// delivery, and an image from a stranger is worse than a line of text.
    void onMediaReceived(const PeerId&, const protocol::ChatMediaPayload&);

    void onReadReceipt(const PeerId&, const ClientMessageId&);

private:
    /// Joins one Chat with its contact name and last message.
    ChatSummary summarise(const Chat&);

    /// PermissionDenied when we blocked this peer. Shared by send() and
    /// sendMedia() so a photo cannot slip past a block that stops text.
    Result refuseIfBlocked(const PeerId&);

    IMessageRepository& messages_;
    IContactRepository& contacts_;
    DeliveryService&    delivery_;
    MeshEngine&         mesh_;
    IEventSink&         events_;
    IClock&             clock_;
};

}  // namespace anytime
