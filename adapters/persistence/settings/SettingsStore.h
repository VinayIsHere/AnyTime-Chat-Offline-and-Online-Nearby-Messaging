// adapters/persistence/settings/SettingsStore.h
//
// A QSettings-backed account repository plus an in-memory peer/contact store.
//
// WHY THIS EXISTS: the sign-in and Nearby milestone needs persistence for
// exactly one thing — the account — and peers are ephemeral by definition (they
// live inside a 60 s reachability window and are re-learned from announces on
// every launch). Standing up SQLite, migrations and at-rest encryption to store
// one row would have delayed the vertical slice for no benefit.
//
// It implements the same interfaces SqliteStore will, so swapping it out is a
// change in main.cpp and nowhere else.
//
// WHAT IS AND IS NOT DURABLE HERE — read this before trusting it:
//
//   account    PERSISTED to the INI file
//   everything else  RAM ONLY. Chats, messages, contacts, requests and the
//                    outbox are lost on restart.
//
// For peers that is correct — they expire after 60 seconds and are re-learned
// from announces on every launch. For messages and contacts it is NOT: chat
// history that vanishes on restart is not chat history, and a lost pinned key
// means a contact has to be re-accepted.
//
// This is the honest state of the messaging milestone: the logic is complete
// and exercised end to end, and durability is the piece still owed. SQLite
// replaces this class wholesale and no interface changes when it does.

#pragma once

#include <QString>

#include <memory>

#include "core/ports/IRepositories.h"

namespace anytime::persistence {

class SettingsStore final : public IAccountRepository,
                            public IContactRepository,
                            public IMessageRepository,
                            public IOutbox {
public:
    /// `path` is an INI file; QSettings owns the format and the atomic write.
    explicit SettingsStore(QString path);
    ~SettingsStore() override;

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

    Result setBlocked(const PeerId&, bool) override;
    bool   isBlocked(const PeerId&) override;
    Outcome<std::vector<BlockedPeer>> listBlocked() override;

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
    Result                          putMedia(const std::string&, ByteSpan) override;
    Outcome<ByteBuffer>             getMedia(const std::string&) override;

    // IOutbox
    Result                            enqueue(const OutboxEntry&) override;
    Result                            remove(const MessageId&) override;
    Outcome<std::vector<OutboxEntry>> dueFor(const PeerId&, Timestamp) override;
    Outcome<std::vector<PeerId>>      peersWithPending() override;
    Result                            recordAttempt(const MessageId&, Timestamp) override;
    Outcome<std::vector<OutboxEntry>> reapExpired(Timestamp) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::persistence
