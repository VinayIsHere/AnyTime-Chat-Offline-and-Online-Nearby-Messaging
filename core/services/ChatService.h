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

    /// GET /{t}/api/chats/:id/messages?before=<cursor>
    Outcome<MessagePage> history(const ChatId&, const Cursor&, int limit = 50);

    /// POST /{t}/api/chats/:id/messages — returns the row at status Queued.
    Outcome<Message> send(const ChatId&, const std::string& body,
                          const ClientMessageId&);

    /// Marks messages read up to and including `messageId`, clears the unread
    /// count and sends a chat.read receipt.
    Result markRead(const ChatId&, const MessageId&);

    // ── driven by MeshEngine ─────────────────────────────────────────────────

    /// Stores an inbound message and emits message.received. Drops messages from
    /// peers with no accepted contact — the request gate is enforced here too,
    /// not only at request time.
    void onMessageReceived(const PeerId&, const protocol::ChatMessagePayload&);

    void onReadReceipt(const PeerId&, const ClientMessageId&);

private:
    IMessageRepository& messages_;
    IContactRepository& contacts_;
    DeliveryService&    delivery_;
    MeshEngine&         mesh_;
    IEventSink&         events_;
    IClock&             clock_;
};

}  // namespace anytime
