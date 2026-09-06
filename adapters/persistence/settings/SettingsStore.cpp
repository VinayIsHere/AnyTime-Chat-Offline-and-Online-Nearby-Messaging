// adapters/persistence/settings/SettingsStore.cpp

#include "adapters/persistence/settings/SettingsStore.h"

#include <QSettings>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace anytime::persistence {

namespace {

QString keyHex(const PublicKey& key)
{
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(key.data()), kPublicKeyBytes).toHex());
}

bool keyFromHex(const QString& hex, PublicKey& out)
{
    const QByteArray raw = QByteArray::fromHex(hex.toLatin1());
    if (raw.size() != static_cast<int>(kPublicKeyBytes)) return false;
    std::memcpy(out.data(), raw.constData(), kPublicKeyBytes);
    return true;
}

}  // namespace

struct SettingsStore::Impl {
    QSettings settings;

    // Peers are deliberately RAM-only: they are valid for 60 seconds and are
    // re-learned from announces on every launch, so persisting them would only
    // let the Nearby list show people who are no longer there.
    std::unordered_map<PeerId, Peer>       peers;
    std::unordered_map<PeerId, Contact>    contacts;
    std::unordered_set<PeerId>             blocked;
    std::unordered_map<std::string, Request> requests;

    explicit Impl(const QString& path) : settings(path, QSettings::IniFormat) {}
};

// ─────────────────────────────────────────────────────────────────────────────

SettingsStore::SettingsStore(QString path) : impl_(std::make_unique<Impl>(path)) {}

SettingsStore::~SettingsStore() = default;

// ── IAccountRepository ───────────────────────────────────────────────────────

Outcome<std::optional<Account>> SettingsStore::load()
{
    QSettings& s = impl_->settings;
    if (!s.contains(QStringLiteral("account/peerId"))) {
        return std::optional<Account>{};
    }

    const auto peerId =
        PeerId::fromHex(s.value(QStringLiteral("account/peerId")).toString().toStdString());
    if (!peerId) {
        return Outcome<std::optional<Account>>::error(ErrorCode::StorageFailure,
                                                      "Stored peer id is malformed.");
    }

    Account account;
    account.peerId      = *peerId;
    account.username    = s.value(QStringLiteral("account/username")).toString().toStdString();
    account.dateOfBirth = s.value(QStringLiteral("account/dob")).toString().toStdString();

    account.gender = genderFromString(
                         s.value(QStringLiteral("account/gender")).toString().toStdString())
                         .value_or(Gender::PreferNotToSay);

    account.discoverable = s.value(QStringLiteral("account/discoverable"), true).toBool();
    account.acceptFrom =
        acceptFromString(s.value(QStringLiteral("account/acceptFrom")).toString().toStdString())
            .value_or(AcceptFrom::Everyone);

    account.createdAt = s.value(QStringLiteral("account/createdAt"), 0).toLongLong();

    keyFromHex(s.value(QStringLiteral("account/staticPublicKey")).toString(),
               account.staticPublicKey);
    keyFromHex(s.value(QStringLiteral("account/signingPublicKey")).toString(),
               account.signingPublicKey);

    return std::optional<Account>{account};
}

Result SettingsStore::save(const Account& account)
{
    QSettings& s = impl_->settings;

    s.setValue(QStringLiteral("account/peerId"), QString::fromStdString(account.peerId.toHex()));
    s.setValue(QStringLiteral("account/username"), QString::fromStdString(account.username));
    s.setValue(QStringLiteral("account/dob"), QString::fromStdString(account.dateOfBirth));
    s.setValue(QStringLiteral("account/gender"), QString::fromLatin1(toString(account.gender)));
    s.setValue(QStringLiteral("account/discoverable"), account.discoverable);
    s.setValue(QStringLiteral("account/acceptFrom"),
               account.acceptFrom == AcceptFrom::Everyone ? QStringLiteral("everyone")
                                                          : QStringLiteral("nobody"));
    s.setValue(QStringLiteral("account/createdAt"), static_cast<qlonglong>(account.createdAt));
    s.setValue(QStringLiteral("account/staticPublicKey"), keyHex(account.staticPublicKey));
    s.setValue(QStringLiteral("account/signingPublicKey"), keyHex(account.signingPublicKey));

    s.sync();
    return s.status() == QSettings::NoError
               ? Result::ok()
               : Result::error(ErrorCode::StorageFailure, "Could not write the account.");
}

Result SettingsStore::clear()
{
    impl_->settings.remove(QStringLiteral("account"));
    impl_->settings.sync();
    return Result::ok();
}

// ── IContactRepository ───────────────────────────────────────────────────────

Result SettingsStore::upsertPeer(const Peer& peer)
{
    impl_->peers[peer.peerId] = peer;
    return Result::ok();
}

Outcome<std::optional<Peer>> SettingsStore::findPeer(const PeerId& peerId)
{
    const auto it = impl_->peers.find(peerId);
    if (it == impl_->peers.end()) return std::optional<Peer>{};
    return std::optional<Peer>{it->second};
}

Outcome<std::vector<Peer>> SettingsStore::listPeersSeenSince(Timestamp since)
{
    std::vector<Peer> out;
    for (const auto& [id, peer] : impl_->peers) {
        if (peer.lastSeenAt >= since) out.push_back(peer);
    }
    return out;
}

Result SettingsStore::prunePeersOlderThan(Timestamp cutoff)
{
    for (auto it = impl_->peers.begin(); it != impl_->peers.end();) {
        it = it->second.lastSeenAt < cutoff ? impl_->peers.erase(it) : std::next(it);
    }
    return Result::ok();
}

Result SettingsStore::addContact(const Contact& contact)
{
    impl_->contacts[contact.peerId] = contact;
    return Result::ok();
}

Outcome<std::optional<Contact>> SettingsStore::findContact(const PeerId& peerId)
{
    const auto it = impl_->contacts.find(peerId);
    if (it == impl_->contacts.end()) return std::optional<Contact>{};
    return std::optional<Contact>{it->second};
}

Outcome<std::vector<Contact>> SettingsStore::listContacts()
{
    std::vector<Contact> out;
    out.reserve(impl_->contacts.size());
    for (const auto& [id, contact] : impl_->contacts) out.push_back(contact);
    return out;
}

Result SettingsStore::setBlocked(const PeerId& peerId, bool blocked)
{
    if (blocked) impl_->blocked.insert(peerId);
    else impl_->blocked.erase(peerId);
    return Result::ok();
}

bool SettingsStore::isBlocked(const PeerId& peerId)
{
    return impl_->blocked.count(peerId) > 0;
}

Result SettingsStore::saveRequest(const Request& request)
{
    impl_->requests[request.id.value()] = request;
    return Result::ok();
}

Outcome<std::optional<Request>> SettingsStore::findRequest(const RequestId& id)
{
    const auto it = impl_->requests.find(id.value());
    if (it == impl_->requests.end()) return std::optional<Request>{};
    return std::optional<Request>{it->second};
}

Outcome<std::vector<Request>> SettingsStore::listRequests(Direction direction, RequestState state)
{
    std::vector<Request> out;
    for (const auto& [id, request] : impl_->requests) {
        if (request.direction == direction && request.state == state) out.push_back(request);
    }
    return out;
}

Result SettingsStore::setRequestState(const RequestId& id, RequestState state)
{
    const auto it = impl_->requests.find(id.value());
    if (it == impl_->requests.end()) {
        return Result::error(ErrorCode::NotFound, "No such request.");
    }
    it->second.state = state;
    return Result::ok();
}

Outcome<int> SettingsStore::countInboundRequestsSince(const PeerId& peerId, Timestamp since)
{
    int count = 0;
    for (const auto& [id, request] : impl_->requests) {
        if (request.peerId == peerId && request.direction == Direction::Incoming
            && request.createdAt >= since) {
            ++count;
        }
    }
    return count;
}

}  // namespace anytime::persistence
