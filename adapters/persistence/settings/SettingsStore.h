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
// It implements the same IAccountRepository and IContactRepository interfaces
// SqliteStore will, so swapping it out is a change in main.cpp and nowhere else.
// Messages and the outbox are NOT implemented here — those arrive with SQLite in
// the messaging milestone, which is when durable history first matters.

#pragma once

#include <QString>

#include <memory>

#include "core/ports/IRepositories.h"

namespace anytime::persistence {

class SettingsStore final : public IAccountRepository, public IContactRepository {
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

    Result                          saveRequest(const Request&) override;
    Outcome<std::optional<Request>> findRequest(const RequestId&) override;
    Outcome<std::vector<Request>>   listRequests(Direction, RequestState) override;
    Result                          setRequestState(const RequestId&, RequestState) override;
    Outcome<int> countInboundRequestsSince(const PeerId&, Timestamp) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::persistence
