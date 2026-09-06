// adapters/persistence/sqlite/SqliteStore.h
//
// SQLite implementation of every repository port. Schema: docs/ARCHITECTURE.md §12.
//
// One object implements all four interfaces because accepting a request touches
// contacts, requests and chats atomically and must do so in one transaction.
// Services still depend only on the narrow interface they need.
//
// AT REST (§10): body_encrypted and payload_encrypted hold ChaCha20-Poly1305
// ciphertext under a key from the Android Keystore, so a device backup or
// filesystem dump yields nothing readable. Encryption happens HERE, not in the
// services — the domain deals in plaintext.
//
// Qt Sql vs the amalgamation: Qt Sql is simpler and already in the build, but
// its SQLite driver plugin must be deployed with the app, a known Android
// packaging annoyance. Either sits behind these interfaces, so switching later
// touches nothing above.

#pragma once

#include <QString>

#include <memory>

#include "core/ports/IRepositories.h"

namespace anytime {
class IClock;
}

namespace anytime::persistence {

/// Encrypts message bodies and outbox payloads at rest. Supplied by the identity
/// adapter, which holds the Keystore-wrapped data key.
class IDataCipher {
public:
    virtual ~IDataCipher() = default;
    virtual Result seal(ByteSpan plaintext, ByteBuffer& out)  = 0;
    virtual Result open(ByteSpan ciphertext, ByteBuffer& out) = 0;
};

class SqliteStore final : public IAccountRepository,
                          public IContactRepository,
                          public IMessageRepository,
                          public IOutbox {
public:
    SqliteStore(QString databasePath, IDataCipher&, IClock&);
    ~SqliteStore() override;

    /// Opens the database, enables WAL, and runs pending migrations.
    Result open();
    void   close();

    /// Current user_version. Migrations are forward-only and idempotent.
    int schemaVersion() const;

    /// Panic wipe — drops every table and vacuums.
    Result wipeAll();

    // IAccountRepository
    Outcome<std::optional<Account>> load() override;
    Result save(const Account&) override;
    Result clear() override;

    // IContactRepository
    Result                        upsertPeer(const Peer&) override;
    Outcome<std::optional<Peer>>  findPeer(const PeerId&) override;
    Outcome<std::vector<Peer>>    listPeersSeenSince(Timestamp) override;
    Result                        prunePeersOlderThan(Timestamp) override;
    Result                          addContact(const Contact&) override;
    Outcome<std::optional<Contact>> findContact(const PeerId&) override;
    Outcome<std::vector<Contact>>   listContacts() override;
    Result                          setBlocked(const PeerId&, bool) override;
    bool                            isBlocked(const PeerId&) override;
    Result                          saveRequest(const Request&) override;
    Outcome<std::optional<Request>> findRequest(const RequestId&) override;
    Outcome<std::vector<Request>>   listRequests(Direction, RequestState) override;
    Result                          setRequestState(const RequestId&, RequestState) override;
    Outcome<int> countInboundRequestsSince(const PeerId&, Timestamp) override;

    // IMessageRepository
    Outcome<Chat>                createChat(const PeerId&) override;
    Outcome<std::optional<Chat>> findChat(const ChatId&) override;
    Outcome<std::optional<Chat>> findChatByPeer(const PeerId&) override;
    Outcome<std::vector<Chat>>   listChats() override;
    Result                       setUnreadCount(const ChatId&, int) override;
    Outcome<MessageId>           append(const Message&) override;
    Result                       setStatus(const MessageId&, DeliveryStatus) override;
    Outcome<MessagePage>         page(const ChatId&, const Cursor&, int) override;
    Outcome<std::optional<Message>> findMessage(const MessageId&) override;
    Outcome<std::optional<Message>> findByClientId(const ClientMessageId&) override;

    // IOutbox
    Result                             enqueue(const OutboxEntry&) override;
    Result                             remove(const MessageId&) override;
    Outcome<std::vector<OutboxEntry>>  dueFor(const PeerId&, Timestamp) override;
    Outcome<std::vector<PeerId>>       peersWithPending() override;
    Result                             recordAttempt(const MessageId&, Timestamp) override;
    Outcome<std::vector<OutboxEntry>>  reapExpired(Timestamp) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::persistence
