// core/ports/IRepositories.h
//
// Persistence ports. Implemented in adapters/persistence/sqlite against the
// schema in docs/ARCHITECTURE.md §12.
//
// Grouped in one header because they share the same lifetime, the same
// transaction scope and the same implementation object. They remain separate
// interfaces so a service depends only on what it actually uses.

#pragma once

#include <functional>
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

    /// Every block, most recent first, each with the best name we hold for it.
    virtual Outcome<std::vector<BlockedPeer>> listBlocked() = 0;

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

    // Media blobs, addressed separately from the message rows that reference
    // them. Kept out of the message table on purpose — see Message::mediaId.
    //
    // Implementations encrypt at rest exactly as they do message bodies: a photo
    // is the most sensitive thing this app stores, and it is the thing most
    // obviously recoverable from a filesystem dump if it is not.

    virtual Result putMedia(const std::string& mediaId, ByteSpan bytes) = 0;

    /// NotFound when the id is unknown — which includes an image whose sender
    /// never finished transferring it.
    virtual Outcome<ByteBuffer> getMedia(const std::string& mediaId) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────

/// Makes several repository writes one atomic unit.
///
/// Accepting a request writes a contact, a chat and a request state. Separately,
/// a crash between them left a contact with no chat — and because the peer was
/// now "already a contact", every later request from them was ignored, so the
/// pair could never recover. Inside one transaction that state cannot exist.
///
/// A separate interface rather than a method on each repository because the
/// unit spans them: the writes above touch IContactRepository AND
/// IMessageRepository, which only works because one object implements both
/// over one connection. An implementation that split them across stores
/// could not honour this port, and should not claim to.
class ITransactionScope {
public:
    virtual ~ITransactionScope() = default;

    /// Runs `work`. Commits if it returns Ok; rolls back if it returns an error
    /// or the commit itself fails, and returns that error either way.
    ///
    /// Calls made while a transaction is already open JOIN it rather than
    /// nesting: an inner failure then fails the whole outer unit, which is the
    /// only honest outcome when the inner writes cannot be undone alone.
    ///
    /// Events and radio sends belong AFTER this returns Ok, never inside `work`:
    /// telling the UI or a peer about a write that is then rolled back would be
    /// a lie that cannot be taken back.
    virtual Result runInTransaction(const std::function<Result()>& work) = 0;
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
