// adapters/persistence/settings/SettingsStore.cpp

#include "adapters/persistence/settings/SettingsStore.h"

#include <QDateTime>
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

    std::unordered_map<std::string, Chat>       chats;      // by chat id
    std::vector<Message>                        messages;   // append order
    std::unordered_map<std::string, ByteBuffer> media;      // by media id
    std::unordered_map<std::string, OutboxEntry> outbox;    // by message id
    std::uint64_t                               nextMessageSeq = 1;

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

Outcome<std::vector<BlockedPeer>> SettingsStore::listBlocked()
{
    // This store keeps no block timestamp; it survives only to import an old
    // account, so names from the contact map are as much as it can offer.
    std::vector<BlockedPeer> out;
    out.reserve(impl_->blocked.size());
    for (const PeerId& peerId : impl_->blocked) {
        BlockedPeer row;
        row.peerId = peerId;
        const auto contact = impl_->contacts.find(peerId);
        if (contact != impl_->contacts.end()) row.displayName = contact->second.displayName;
        out.push_back(row);
    }
    return out;
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


// ─────────────────────────────────────────────────────────────────────────────
// IMessageRepository
// ─────────────────────────────────────────────────────────────────────────────

namespace {

QString messageKey(int index, const char* field)
{
    return QStringLiteral("messages/%1/%2").arg(index).arg(QLatin1String(field));
}

}  // namespace

Outcome<Chat> SettingsStore::createChat(const PeerId& peerId)
{
    // One chat per peer, always. Two chats with the same person would split the
    // history in a way nothing downstream expects.
    for (const auto& [id, chat] : impl_->chats) {
        if (chat.peerId == peerId) return chat;
    }

    Chat chat;
    chat.id = ChatId{"chat_" + peerId.toHex()};
    chat.peerId = peerId;
    chat.createdAt = QDateTime::currentMSecsSinceEpoch();
    chat.lastMessageAt = chat.createdAt;
    chat.unreadCount = 0;

    impl_->chats[chat.id.value()] = chat;
    return chat;
}

Outcome<std::optional<Chat>> SettingsStore::findChat(const ChatId& id)
{
    const auto it = impl_->chats.find(id.value());
    if (it == impl_->chats.end()) return std::optional<Chat>{};
    return std::optional<Chat>{it->second};
}

Outcome<std::optional<Chat>> SettingsStore::findChatByPeer(const PeerId& peerId)
{
    for (const auto& [id, chat] : impl_->chats) {
        if (chat.peerId == peerId) return std::optional<Chat>{chat};
    }
    return std::optional<Chat>{};
}

Outcome<std::vector<Chat>> SettingsStore::listChats()
{
    std::vector<Chat> out;
    out.reserve(impl_->chats.size());
    for (const auto& [id, chat] : impl_->chats) out.push_back(chat);

    std::sort(out.begin(), out.end(), [](const Chat& a, const Chat& b) {
        return a.lastMessageAt > b.lastMessageAt;  // most recent first
    });
    return out;
}

Result SettingsStore::setUnreadCount(const ChatId& id, int count)
{
    const auto it = impl_->chats.find(id.value());
    if (it == impl_->chats.end()) return Result::error(ErrorCode::NotFound, "No such chat.");
    it->second.unreadCount = count;
    return Result::ok();
}

Outcome<MessageId> SettingsStore::append(const Message& message)
{
    Message stored = message;
    stored.id = MessageId{"msg_" + std::to_string(impl_->nextMessageSeq++)};

    impl_->messages.push_back(stored);

    const auto chat = impl_->chats.find(stored.chatId.value());
    if (chat != impl_->chats.end()) {
        chat->second.lastMessageAt = stored.createdAt;
        }
    return stored.id;
}

Result SettingsStore::setStatus(const MessageId& id, DeliveryStatus status)
{
    for (Message& message : impl_->messages) {
        if (message.id != id) continue;
        message.status = status;
        if (status == DeliveryStatus::Delivered && message.deliveredAt == kNoTimestamp) {
            message.deliveredAt = QDateTime::currentMSecsSinceEpoch();
        }
        if (status == DeliveryStatus::Read && message.readAt == kNoTimestamp) {
            message.readAt = QDateTime::currentMSecsSinceEpoch();
        }
            return Result::ok();
    }
    return Result::error(ErrorCode::NotFound, "No such message.");
}

Outcome<MessagePage> SettingsStore::page(const ChatId& chatId, const Cursor& cursor, int limit)
{
    std::vector<Message> inChat;
    for (const Message& message : impl_->messages) {
        if (message.chatId == chatId) inChat.push_back(message);
    }

    // Newest first, which is what the UI renders and reverses.
    std::sort(inChat.begin(), inChat.end(), [](const Message& a, const Message& b) {
        return a.createdAt > b.createdAt;
    });

    std::size_t start = 0;
    if (!cursor.empty()) {
        // The cursor is the id of the last message the client already has.
        for (std::size_t i = 0; i < inChat.size(); ++i) {
            if (inChat[i].id.value() == cursor) { start = i + 1; break; }
        }
    }

    MessagePage result;
    for (std::size_t i = start; i < inChat.size() && result.messages.size() < std::size_t(limit);
         ++i) {
        result.messages.push_back(inChat[i]);
    }
    result.hasMore = (start + result.messages.size()) < inChat.size();
    if (result.hasMore && !result.messages.empty()) {
        result.nextCursor = result.messages.back().id.value();
    }
    return result;
}

Outcome<std::optional<Message>> SettingsStore::findMessage(const MessageId& id)
{
    for (const Message& message : impl_->messages) {
        if (message.id == id) return std::optional<Message>{message};
    }
    return std::optional<Message>{};
}

Outcome<std::optional<Message>> SettingsStore::findByClientId(const ClientMessageId& clientId)
{
    for (const Message& message : impl_->messages) {
        if (message.clientMessageId == clientId) return std::optional<Message>{message};
    }
    return std::optional<Message>{};
}

// Media, like everything else here except the account, is RAM-only and NOT
// encrypted. That is acceptable only because this class exists solely to import
// a legacy account into SqliteStore at start-up; it must never carry real
// images. SqliteStore is the implementation that ships.

Result SettingsStore::putMedia(const std::string& mediaId, ByteSpan bytes)
{
    if (mediaId.empty()) return Result::error(ErrorCode::InvalidArgument, "Media id is empty.");
    impl_->media[mediaId] = ByteBuffer(bytes.begin(), bytes.end());
    return Result::ok();
}

Outcome<ByteBuffer> SettingsStore::getMedia(const std::string& mediaId)
{
    const auto it = impl_->media.find(mediaId);
    if (it == impl_->media.end()) {
        return Outcome<ByteBuffer>::error(ErrorCode::NotFound, "No such image.");
    }
    return it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// IOutbox
// ─────────────────────────────────────────────────────────────────────────────

Result SettingsStore::enqueue(const OutboxEntry& entry)
{
    impl_->outbox[entry.messageId.value()] = entry;
    return Result::ok();
}

Result SettingsStore::remove(const MessageId& id)
{
    impl_->outbox.erase(id.value());
    return Result::ok();
}

Outcome<std::vector<OutboxEntry>> SettingsStore::dueFor(const PeerId& peerId, Timestamp now)
{
    std::vector<OutboxEntry> due;
    for (const auto& [id, entry] : impl_->outbox) {
        if (entry.peerId == peerId && entry.nextAttemptAt <= now) due.push_back(entry);
    }
    // Oldest first: a chat delivered out of order reads as nonsense.
    std::sort(due.begin(), due.end(), [](const OutboxEntry& a, const OutboxEntry& b) {
        return a.nextAttemptAt < b.nextAttemptAt;
    });
    return due;
}

Outcome<std::vector<PeerId>> SettingsStore::peersWithPending()
{
    std::vector<PeerId> peers;
    for (const auto& [id, entry] : impl_->outbox) {
        bool seen = false;
        for (const PeerId& known : peers) {
            if (known == entry.peerId) { seen = true; break; }
        }
        if (!seen) peers.push_back(entry.peerId);
    }
    return peers;
}

Result SettingsStore::recordAttempt(const MessageId& id, Timestamp nextAttemptAt)
{
    const auto it = impl_->outbox.find(id.value());
    if (it == impl_->outbox.end()) return Result::error(ErrorCode::NotFound, "Not in the outbox.");
    ++it->second.attempts;
    it->second.nextAttemptAt = nextAttemptAt;
    return Result::ok();
}

Outcome<std::vector<OutboxEntry>> SettingsStore::reapExpired(Timestamp now)
{
    std::vector<OutboxEntry> expired;
    for (auto it = impl_->outbox.begin(); it != impl_->outbox.end();) {
        const bool tooManyAttempts = it->second.attempts >= kMaxSendAttempts;
        const bool tooOld = it->second.expiresAt != kNoTimestamp && now >= it->second.expiresAt;

        if (tooManyAttempts || tooOld) {
            expired.push_back(it->second);
            it = impl_->outbox.erase(it);
        } else {
            ++it;
        }
    }
    return expired;
}

}  // namespace anytime::persistence
