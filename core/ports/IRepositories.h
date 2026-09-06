// core/ports/IRepositories.h
//
// Persistence ports. Implemented in adapters/persistence/sqlite against the
// schema in docs/ARCHITECTURE.md §12.
//
// Grouped in one header because they share the same lifetime, the same
// transaction scope and the same implementation object. They remain separate
// interfaces so a service depends only on what it actually uses.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/domain/Entities.h"
#include "core/domain/Result.h"
#include "core/domain/Types.h"

namespace anytime {

// ─────────────────────────────────────────────────────────────────────────────

class IAccountRepository {
public:
    virtual ~IAccountRepository() = default;

    /// nullopt before setup — the UI routes to /welcome in that case.
    virtual Outcome<std::optional<Account>> load() = 0;

    virtual Result save(const Account&) = 0;
    virtual Result clear() = 0;  ///< part of panic wipe
};

// ─────────────────────────────────────────────────────────────────────────────

/// Peers, contacts and the block list. One interface because accepting a
/// request touches all three atomically.
class IContactRepository {
public:
    virtual ~IContactRepository() = default;

    // Peers — ephemeral discovery state
    virtual Result                        upsertPeer(const Peer&)        = 0;
    virtual Outcome<std::optional<Peer>>  findPeer(const PeerId&)        = 0;
    virtual Outcome<std::vector<Peer>>    listPeersSeenSince(Timestamp)  = 0;
    virtual Result                        prunePeersOlderThan(Timestamp) = 0;

    // Contacts — durable, key-pinned
    virtual Result                          addContact(const Contact&)  = 0;
    virtual Outcome<std::optional<Contact>> findContact(const PeerId&)  = 0;
    virtual Outcome<std::vector<Contact>>   listContacts()              = 0;

    /// Blocking is keyed on PeerId (the key fingerprint), never on nickname,
    /// so it survives the peer renaming themselves (§08).
    virtual Result setBlocked(const PeerId&, bool blocked) = 0;
    virtual bool   isBlocked(const PeerId&)                = 0;

    // Requests
    virtual Result                          saveRequest(const Request&)      = 0;
    virtual Outcome<std::optional<Request>> findRequest(const RequestId&)    = 0;
    virtual Outcome<std::vector<Request>>   listRequests(Direction, RequestState) = 0;
    virtual Result                          setRequestState(const RequestId&, RequestState) = 0;

    /// Rate limiting for inbound requests (§08): how many arrived from this peer
    /// since `since`. The service enforces the ceiling; storage just counts.
    virtual Outcome<int> countInboundRequestsSince(const PeerId&, Timestamp since) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────

class IMessageRepository {
public:
    virtual ~IMessageRepository() = default;

    // Chats
    virtual Outcome<Chat>                createChat(const PeerId&)   = 0;
    virtual Outcome<std::optional<Chat>> findChat(const ChatId&)     = 0;
    virtual Outcome<std::optional<Chat>> findChatByPeer(const PeerId&) = 0;
    virtual Outcome<std::vector<Chat>>   listChats()                 = 0;
    virtual Result                       setUnreadCount(const ChatId&, int) = 0;

    // Messages
    virtual Outcome<MessageId>      append(const Message&)                     = 0;
    virtual Result                  setStatus(const MessageId&, DeliveryStatus) = 0;
    virtual Outcome<MessagePage>    page(const ChatId&, const Cursor&, int limit) = 0;
    virtual Outcome<std::optional<Message>> findMessage(const MessageId&)      = 0;

    /// Reconciles an inbound ack with the optimistic row the UI already rendered.
    virtual Outcome<std::optional<Message>> findByClientId(const ClientMessageId&) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────

/// Durable pending sends. Survives app kill and reboot (§09) — this is what makes
/// the app usable when a peer drifts in and out of range.
class IOutbox {
public:
    virtual ~IOutbox() = default;

    virtual Result enqueue(const OutboxEntry&) = 0;
    virtual Result remove(const MessageId&)    = 0;

    /// Entries for one peer that are due at or before `now`, oldest first.
    /// DeliveryService calls this when DiscoveryService reports the peer back.
    virtual Outcome<std::vector<OutboxEntry>> dueFor(const PeerId&, Timestamp now) = 0;

    /// Peers with any pending mail — drives the drain-on-reappearance check.
    virtual Outcome<std::vector<PeerId>> peersWithPending() = 0;

    virtual Result recordAttempt(const MessageId&, Timestamp nextAttemptAt) = 0;

    /// Drops entries past kMaxSendAttempts or kOutboxTtlMillis and returns them
    /// so the caller can flip their messages to Failed.
    virtual Outcome<std::vector<OutboxEntry>> reapExpired(Timestamp now) = 0;
};

}  // namespace anytime
