// tests/ServiceTests.cpp
//
// The services behind the Profile, Requests and Chat screens, driven through a
// real AppCore: real SQLite, real identity keys, real Noise factory, real CBOR.
// The only fakes are the clock, the event sink and one transport — and the
// transport exists so a test can read what the device actually puts on the air
// rather than trusting a flag it set.
//
// What these guard, in the order a user would notice them missing:
//   - blocking reaches the list, the Requests tab, the Nearby screen and the
//     chat composer, and unblocking undoes all of it
//   - a failed message can be resent, and nothing else can be
//   - renaming yourself, or hiding, changes what the radio broadcasts NOW —
//     not after the next restart, which is what it used to do

#include "tests/TestHarness.h"

#include <QDir>
#include <QTemporaryDir>

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "adapters/codec/QtCborCodec.h"
#include "adapters/crypto/NoiseSessionFactory.h"
#include "adapters/events/FanOutEventSink.h"
#include "adapters/notify/Notifier.h"
#include "adapters/identity/MonocypherIdentityStore.h"
#include "adapters/persistence/sqlite/MonocypherDataCipher.h"
#include "adapters/persistence/sqlite/SqliteStore.h"
#include "core/AppCore.h"
#include "core/ports/IClock.h"
#include "core/ports/IEventSink.h"
#include "core/ports/ITransport.h"
#include "core/protocol/PacketCodec.h"
#include "core/services/AccountService.h"
#include "core/services/BlockService.h"
#include "core/services/ChatService.h"
#include "core/services/DiscoveryService.h"
#include "core/services/RequestService.h"

namespace anytime::test {

namespace {

/// Timers are held rather than run, so a test decides exactly when the next
/// announce goes out instead of sleeping for it.
class ManualClock final : public IClock {
public:
    Timestamp now() const override { return now_; }
    Timestamp monotonic() const override { return now_; }

    TimerId scheduleOnce(Timestamp, std::function<void()> fn) override
    {
        const TimerId id = next_++;
        once_[id] = std::move(fn);
        return id;
    }
    TimerId scheduleRepeating(Timestamp, std::function<void()>) override { return next_++; }
    void    cancel(TimerId id) override { once_.erase(id); }

    /// Runs every one-shot timer pending right now. Timers they schedule wait
    /// for the next call, so a self-rescheduling announce fires exactly once.
    void runPending()
    {
        auto due = std::move(once_);
        once_.clear();
        for (auto& [id, fn] : due) fn();
    }

    Timestamp now_ = 1'700'000'000'000;

private:
    TimerId                                  next_ = 1;
    std::map<TimerId, std::function<void()>> once_;
};

class RecordingEvents final : public IEventSink {
public:
    void onPeerDiscovered(const Peer&) override {}
    void onPeerLost(const PeerId& peerId) override { lost.push_back(peerId); }
    void onRequestReceived(const Request&) override {}
    void onRequestAccepted(const Request&, const Chat& chat) override { accepted.push_back(chat); }
    void onRequestDeclined(const RequestId&, const PeerId&) override {}
    void onMessageReceived(const Message& m) override { received.push_back(m); }
    void onMessageStatusChanged(const MessageId& id, DeliveryStatus status) override
    {
        statuses.emplace_back(id, status);
    }
    void onRadioStateChanged(bool, const std::string&) override {}

    std::vector<PeerId>                               lost;
    std::vector<std::pair<MessageId, DeliveryStatus>> statuses;
    std::vector<Chat>                                 accepted;
    std::vector<Message>                              received;
};

// ─────────────────────────────────────────────────────────────────────────────
// The air between two devices
// ─────────────────────────────────────────────────────────────────────────────

class AirTransport;

/// An in-memory radio medium. Frames are QUEUED, never delivered inside send():
/// real radios are asynchronous, and delivering re-entrantly would let a reply
/// arrive before the send that caused it had returned — an ordering no phone
/// can produce, and one the core is right not to handle.
///
/// Every frame is also recorded, so a test can look at exactly what a passive
/// listener nearby would have seen.
class Air {
public:
    struct Heard {
        PeerId     from;
        ByteBuffer frame;
    };

    void join(AirTransport* radio) { radios_.push_back(radio); }

    /// Every started radio sights every other — both directions, as when two
    /// phones come into range.
    void sight();

    /// Delivers until nothing is in flight. Returns how many frames moved.
    int pump();

    void carry(AirTransport* to, LinkId link, ByteBuffer frame, const PeerId& from)
    {
        heard.push_back(Heard{from, frame});
        inFlight_.push_back(Delivery{to, link, std::move(frame), false, PeerId{}});
    }
    void announceLinkUp(AirTransport* to, LinkId link, const PeerId& remote)
    {
        inFlight_.push_back(Delivery{to, link, ByteBuffer{}, true, remote});
    }

    AirTransport* radioFor(const PeerId& peerId) const;

    std::vector<Heard> heard;

private:
    struct Delivery {
        AirTransport* to;
        LinkId        link;
        ByteBuffer    frame;
        bool          linkUp;
        PeerId        remote;
    };
    std::vector<AirTransport*> radios_;
    std::deque<Delivery>       inFlight_;
};

/// BLE-shaped: a 480-byte frame budget, so anything bigger — a photo — goes
/// through the core's fragmenter exactly as it would on a phone.
class AirTransport final : public ITransport {
public:
    explicit AirTransport(Air& air) : air_(air) { air_.join(this); }

    TransportId id() const override { return TransportId::Loopback; }
    TransportCapabilities caps() const override
    {
        TransportCapabilities c;
        c.maxFramePayload = protocol::kFragmentPayloadBytes;
        c.supportsMesh    = true;
        return c;
    }
    bool isAvailable() const override { return started_; }

    Result start(const PeerId& localPeerId) override
    {
        local_   = localPeerId;
        started_ = true;
        return Result::ok();
    }
    void stop() override { started_ = false; }

    Result startDiscovery(DiscoveryMode) override { return Result::ok(); }
    void   stopDiscovery() override {}
    Result startAdvertising() override { return Result::ok(); }
    void   stopAdvertising() override {}

    Outcome<LinkId> openLink(const PeerHandle& handle) override
    {
        AirTransport* remote = air_.radioFor(handle.peerId);
        if (remote == nullptr) {
            return Outcome<LinkId>::error(ErrorCode::LinkClosed, "Nobody there.");
        }
        const LinkId mine   = nextLink_++;
        const LinkId theirs = remote->nextLink_++;
        links_[mine]           = {remote, theirs};
        remote->links_[theirs] = {this, mine};

        // Like LoopbackTransport: our side is up at once, theirs when the
        // connection reaches them.
        if (listener_) listener_->onLinkUp(id(), mine, remote->local_);
        air_.announceLinkUp(remote, theirs, local_);
        return mine;
    }
    void closeLink(LinkId link) override { links_.erase(link); }

    Result send(LinkId link, ByteSpan frame) override
    {
        // As strict as the real transports: an oversized frame is a core bug.
        if (frame.size() > caps().maxFramePayload) {
            return Result::error(ErrorCode::InvalidArgument, "Frame exceeds the transport budget.");
        }
        if (link == kInvalidLink) {
            for (auto& [mine, peer] : links_) {
                air_.carry(peer.first, peer.second, frame.toBuffer(), local_);
            }
            return Result::ok();
        }
        const auto it = links_.find(link);
        if (it == links_.end()) return Result::error(ErrorCode::LinkClosed, "No such link.");
        air_.carry(it->second.first, it->second.second, frame.toBuffer(), local_);
        return Result::ok();
    }
    void setListener(ITransportListener* listener) override { listener_ = listener; }

    // Driven by the Air.
    void deliver(LinkId link, ByteSpan frame)
    {
        if (listener_) listener_->onFrame(id(), link, frame);
    }
    void linkUp(LinkId link, const PeerId& remote)
    {
        if (listener_) listener_->onLinkUp(id(), link, remote);
    }
    void sightings(const std::vector<AirTransport*>& others)
    {
        if (!started_ || !listener_) return;
        for (AirTransport* other : others) {
            if (other == this || !other->started_) continue;
            PeerHandle handle;
            handle.peerId  = other->local_;
            handle.address = other->local_.toHex();
            handle.rssi    = -50;
            listener_->onPeerSeen(id(), handle);
        }
    }

    const PeerId& local() const { return local_; }

private:
    Air&                                                    air_;
    ITransportListener*                                     listener_ = nullptr;
    PeerId                                                  local_;
    bool                                                    started_ = false;
    LinkId                                                  nextLink_ = 1;
    std::map<LinkId, std::pair<AirTransport*, LinkId>>      links_;
};

void Air::sight()
{
    for (AirTransport* radio : radios_) radio->sightings(radios_);
}

int Air::pump()
{
    int moved = 0;
    // Bounded, so a feedback loop in the core fails the test instead of hanging it.
    while (!inFlight_.empty() && moved < 100000) {
        Delivery d = std::move(inFlight_.front());
        inFlight_.pop_front();
        if (d.linkUp) d.to->linkUp(d.link, d.remote);
        else          d.to->deliver(d.link, ByteSpan{d.frame});
        ++moved;
    }
    return moved;
}

AirTransport* Air::radioFor(const PeerId& peerId) const
{
    for (AirTransport* radio : radios_) {
        if (radio->local() == peerId) return radio;
    }
    return nullptr;
}

/// Stands in for the Android notification shade.
class RecordingPresenter final : public notify::INotificationPresenter {
public:
    void show(const notify::Notice& notice) override { shown.push_back(notice); }
    std::vector<notify::Notice> shown;
};

/// Passes everything to the real store, except that createChat can be told to
/// fail — standing in for a crash or a full disk at the worst possible moment,
/// between the contact being written and the chat.
class FlakyMessages final : public IMessageRepository {
public:
    explicit FlakyMessages(IMessageRepository& real) : real_(real) {}

    bool failCreateChat = false;

    Outcome<Chat> createChat(const PeerId& p) override
    {
        if (failCreateChat) return Outcome<Chat>::error(ErrorCode::StorageFailure, "disk full");
        return real_.createChat(p);
    }
    Outcome<std::optional<Chat>> findChat(const ChatId& c) override { return real_.findChat(c); }
    Outcome<std::optional<Chat>> findChatByPeer(const PeerId& p) override { return real_.findChatByPeer(p); }
    Outcome<std::vector<Chat>>   listChats() override { return real_.listChats(); }
    Result setUnreadCount(const ChatId& c, int n) override { return real_.setUnreadCount(c, n); }
    Outcome<MessageId> append(const Message& m) override { return real_.append(m); }
    Result setStatus(const MessageId& m, DeliveryStatus s) override { return real_.setStatus(m, s); }
    Outcome<MessagePage> page(const ChatId& c, const Cursor& cur, int l) override { return real_.page(c, cur, l); }
    Outcome<std::optional<Message>> findMessage(const MessageId& m) override { return real_.findMessage(m); }
    Outcome<std::optional<Message>> findByClientId(const ClientMessageId& c) override { return real_.findByClientId(c); }
    Result putMedia(const std::string& id, ByteSpan b) override { return real_.putMedia(id, b); }
    Outcome<ByteBuffer> getMedia(const std::string& id) override { return real_.getMedia(id); }

private:
    IMessageRepository& real_;
};

/// A radio that is always on and remembers what it was asked to do. State
/// lives in a shared struct because AppCore takes ownership of the transport.
struct RadioLog {
    bool                    advertising = false;
    std::vector<ByteBuffer> broadcasts;
};

class CapturingTransport final : public ITransport {
public:
    explicit CapturingTransport(std::shared_ptr<RadioLog> log) : log_(std::move(log)) {}

    TransportId id() const override { return TransportId::Loopback; }
    TransportCapabilities caps() const override
    {
        TransportCapabilities c;
        c.maxFramePayload = 4096;
        c.supportsMesh    = true;
        return c;
    }
    bool isAvailable() const override { return started_; }

    Result start(const PeerId&) override { started_ = true; return Result::ok(); }
    void   stop() override { started_ = false; }

    Result startDiscovery(DiscoveryMode) override { return Result::ok(); }
    void   stopDiscovery() override {}

    Result startAdvertising() override { log_->advertising = true; return Result::ok(); }
    void   stopAdvertising() override { log_->advertising = false; }

    Outcome<LinkId> openLink(const PeerHandle&) override
    {
        return Outcome<LinkId>::error(ErrorCode::TransportUnavailable, "No links in this test.");
    }
    void closeLink(LinkId) override {}

    Result send(LinkId link, ByteSpan frame) override
    {
        if (link == kInvalidLink) log_->broadcasts.emplace_back(frame.begin(), frame.end());
        return Result::ok();
    }
    void setListener(ITransportListener*) override {}

private:
    std::shared_ptr<RadioLog> log_;
    bool                      started_ = false;
};

/// The nickname inside the newest ANNOUNCE this device broadcast, or nullopt if
/// it has broadcast none. Parsed from the wire bytes on purpose: the question is
/// what other phones will read, not what the engine believes it is sending.
std::optional<std::string> lastAnnouncedNickname(const RadioLog& log)
{
    for (auto it = log.broadcasts.rbegin(); it != log.broadcasts.rend(); ++it) {
        protocol::Packet packet;
        if (protocol::PacketCodec::decode(ByteSpan{*it}, packet).isError()) continue;
        if (packet.type != protocol::PacketType::Announce || packet.payload.empty()) continue;

        // [ nicknameLen : u8 ][ nickname ][ keys… ] — see MeshEngine.cpp.
        const std::size_t length = packet.payload[0];
        if (packet.payload.size() < 1 + length) continue;
        return std::string(reinterpret_cast<const char*>(packet.payload.data()) + 1, length);
    }
    return std::nullopt;
}

PeerId peerFrom(const char* hex) { return PeerId::fromHex(hex).value_or(PeerId{}); }

PublicKey filledKey(Byte value)
{
    PublicKey key{};
    key.fill(value);
    return key;
}

/// One complete device: storage, identity, crypto and a core wired together the
/// way app/main.cpp does it, minus the HTTP server and the real radios.
struct Device {
    /// With `air`, the device talks to others on it; without, it gets a
    /// capturing radio that only records broadcasts.
    explicit Device(const QString& dir, Air* air = nullptr)
        : cipher(dir + QStringLiteral("/data.key")),
          store(dir + QStringLiteral("/anytime.db"), cipher, clock),
          flaky(store),
          identity(dir + QStringLiteral("/identity.key")),
          sessions(identity),
          radio(std::make_shared<RadioLog>())
    {
        cipher.loadOrCreate();
        store.open();

        AppCoreDeps deps;
        deps.accounts = &store;
        deps.contacts = &store;
        deps.messages = &flaky;
        deps.outbox   = &store;
        deps.transactions = &store;
        deps.identity = &identity;
        deps.sessions = &sessions;
        deps.cbor     = &cbor;
        deps.events   = &fanout;  // as on Android: the page's sink AND the notifier
        deps.clock    = &clock;

        core = std::make_unique<AppCore>(deps);
        if (air != nullptr) core->addTransport(std::make_unique<AirTransport>(*air));
        else                core->addTransport(std::make_unique<CapturingTransport>(radio));
    }

    /// Creates the account and brings the mesh up, as POST /account does.
    bool signUp(const std::string& username)
    {
        CreateAccountRequest request;
        request.username    = username;
        request.dateOfBirth = "1998-04-12";
        request.gender      = Gender::Male;
        if (core->accounts().create(request).isError()) return false;
        return core->startAfterAccountCreated().isOk();
    }

    PeerId self() const
    {
        auto account = core->accounts().current();
        return account.isOk() && account.value().has_value() ? account.value()->peerId : PeerId{};
    }

    /// A contact with a chat, as if a request had been accepted earlier.
    ChatId befriend(const PeerId& peerId, const std::string& name)
    {
        Contact contact;
        contact.peerId          = peerId;
        contact.displayName     = name;
        contact.pinnedStaticKey = filledKey(0x42);
        contact.createdAt       = clock.now();
        store.addContact(contact);
        auto chat = store.createChat(peerId);
        return chat.isOk() ? chat.value().id : ChatId{};
    }

    void receiveRequest(const PeerId& peerId, const std::string& nickname)
    {
        core->requests().onRequestReceived(peerId, nickname, "hi");
    }

    MessageId storeMessage(const ChatId& chatId, Direction direction, DeliveryStatus status,
                           const std::string& clientId)
    {
        Message message;
        message.chatId          = chatId;
        message.clientMessageId = ClientMessageId{clientId};
        message.direction       = direction;
        message.body            = "hello";
        message.status          = status;
        message.createdAt       = clock.now();
        auto id = store.append(message);
        return id.isOk() ? id.value() : MessageId{};
    }

    DeliveryStatus statusOf(const MessageId& id)
    {
        auto found = store.findMessage(id);
        return found.isOk() && found.value().has_value() ? found.value()->status
                                                         : DeliveryStatus::Failed;
    }

    ManualClock                         clock;
    persistence::MonocypherDataCipher   cipher;
    persistence::SqliteStore            store;
    FlakyMessages                       flaky;
    identity::MonocypherIdentityStore   identity;
    crypto::NoiseSessionFactory         sessions;
    codec::QtCborCodec                  cbor;
    RecordingEvents                     events;
    RecordingPresenter                  shade;
    notify::Notifier                    notifier{flaky, store, shade};
    events::FanOutEventSink             fanout{&events, &notifier};
    std::shared_ptr<RadioLog>           radio;
    std::unique_ptr<AppCore>            core;
};

bool listed(const std::vector<BlockedPeer>& list, const PeerId& peerId)
{
    for (const BlockedPeer& row : list) {
        if (row.peerId == peerId) return true;
    }
    return false;
}

std::string nameIn(const std::vector<BlockedPeer>& list, const PeerId& peerId)
{
    for (const BlockedPeer& row : list) {
        if (row.peerId == peerId) return row.displayName;
    }
    return "<missing>";
}


/// Lets two phones on the air do whatever they would do next: see each other,
/// connect, announce, handshake, and deliver what is queued. Each round moves
/// both clocks a second, which is what lets the higher peer id's link grace and
/// the announce timer run.
void settle(Air& air, Device& a, Device& b, int rounds = 8)
{
    for (int i = 0; i < rounds; ++i) {
        air.sight();
        air.pump();
        for (Device* d : {&a, &b}) {
            d->clock.now_ += 1000;
            d->clock.runPending();
        }
        air.pump();
    }
}

/// Payload sizes of the encrypted packets `from` put on the air after `since` —
/// the only length a passive listener gets to see.
std::vector<std::size_t> encryptedSizesFrom(const Air& air, const PeerId& from, std::size_t since)
{
    std::vector<std::size_t> sizes;
    for (std::size_t i = since; i < air.heard.size(); ++i) {
        if (air.heard[i].from != from) continue;
        protocol::Packet packet;
        if (protocol::PacketCodec::decode(ByteSpan{air.heard[i].frame}, packet).isError()) continue;
        if (packet.type == protocol::PacketType::NoiseMessage) sizes.push_back(packet.payload.size());
    }
    return sizes;
}

bool hasContact(Device& d, const PeerId& peerId)
{
    auto found = d.store.findContact(peerId);
    return found.isOk() && found.value().has_value();
}

bool receivedBody(const Device& d, const std::string& body)
{
    for (const Message& m : d.events.received) {
        if (m.body == body) return true;
    }
    return false;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

void runServiceTests(Registry& registry)
{
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        registry.check(false, "temporary directory for the service tests");
        return;
    }
    // One device per scenario, so no test can pass on state another left behind.
    int deviceCount = 0;
    auto freshDir = [&]() {
        const QString dir = tempDir.path() + QStringLiteral("/device%1").arg(++deviceCount);
        QDir().mkpath(dir);
        return dir;
    };

    const PeerId alice   = peerFrom("0011223344556677");
    const PeerId mallory = peerFrom("8899aabbccddeeff");
    const PeerId stranger = peerFrom("1234123412341234");

    // ── the block list ───────────────────────────────────────────────────────

    registry.section("Blocking — the list and what a block tidies up");
    {
        Device d(freshDir());
        registry.check(d.signUp("vinay"), "device signs up");

        auto empty = d.core->blocks().list();
        registry.check(empty.isOk() && empty.value().empty(), "nobody is blocked to begin with");

        d.receiveRequest(mallory, "mallory");
        auto inbox = d.core->requests().inbound();
        registry.check(inbox.isOk() && inbox.value().size() == 1,
                       "a request from a stranger waits in the Requests tab");

        registry.check(d.core->blocks().block(mallory).isOk(), "a stranger can be blocked");
        auto after = d.core->requests().inbound();
        registry.check(after.isOk() && after.value().empty(),
                       "blocking clears their waiting request, so it cannot be accepted by accident");

        registry.check(!d.events.lost.empty() && d.events.lost.back() == mallory,
                       "blocking tells the Nearby screen they are gone, without waiting 60 s");

        auto list = d.core->blocks().list();
        registry.check(list.isOk() && list.value().size() == 1 && listed(list.value(), mallory),
                       "the block list shows them");
        registry.check(list.isOk() && nameIn(list.value(), mallory) == "mallory",
                       "a non-contact is named from the request they sent");

        registry.check(d.core->blocks().block(mallory).isOk()
                           && d.core->blocks().list().value().size() == 1,
                       "blocking twice is harmless and lists them once");

        d.receiveRequest(mallory, "mallory");
        registry.check(d.core->requests().inbound().value().empty(),
                       "a new request from a blocked peer never reaches the Requests tab");

        registry.check(d.core->blocks().block(d.self()).isError(), "you cannot block yourself");
        registry.check(d.core->blocks().list().value().size() == 1,
                       "a refused self-block leaves the list untouched");

        registry.check(d.core->blocks().unblock(mallory).isOk(), "they can be unblocked");
        registry.check(d.core->blocks().list().value().empty(), "unblocking removes them");
        registry.check(d.core->blocks().unblock(mallory).isOk(),
                       "unblocking someone not blocked is harmless");

        d.receiveRequest(mallory, "mallory");
        registry.check(d.core->requests().inbound().value().size() == 1,
                       "after unblocking, their requests arrive again");
    }

    registry.section("Blocking — which name the list trusts");
    {
        Device d(freshDir());
        registry.check(d.signUp("vinay"), "device signs up");

        // A contact who later sends a request under another name: the pinned
        // name was fixed against an authenticated key, so it wins.
        d.befriend(alice, "alice");
        Request renamed;
        renamed.id        = RequestId{"req_alice"};
        renamed.peerId    = alice;
        renamed.direction = Direction::Incoming;
        renamed.nickname  = "definitely_not_alice";
        renamed.createdAt = d.clock.now();
        d.store.saveRequest(renamed);

        // Only ever seen in an announce.
        Peer seen;
        seen.peerId   = stranger;
        seen.nickname = "passerby";
        seen.lastSeenAt = d.clock.now();
        d.store.upsertPeer(seen);

        const PeerId unknown = peerFrom("fedcba9876543210");

        d.core->blocks().block(alice);
        d.clock.now_ += 1000;
        d.core->blocks().block(stranger);
        d.clock.now_ += 1000;
        d.core->blocks().block(unknown);

        auto list = d.core->blocks().list();
        registry.check(list.isOk() && list.value().size() == 3, "all three are listed");
        registry.check(list.isOk() && nameIn(list.value(), alice) == "alice",
                       "a contact keeps their PINNED name, not what they call themselves now");
        registry.check(list.isOk() && nameIn(list.value(), stranger) == "passerby",
                       "someone only ever announced is named from the announce");
        registry.check(list.isOk() && nameIn(list.value(), unknown).empty(),
                       "someone never named at all has an empty name, not a guess");
        registry.check(list.isOk() && list.value().front().peerId == unknown
                           && list.value().back().peerId == alice,
                       "the most recent block is listed first");
    }

    registry.section("Blocking — a contact's chat");
    {
        Device d(freshDir());
        registry.check(d.signUp("vinay"), "device signs up");
        const ChatId chat = d.befriend(alice, "alice");

        auto before = d.core->chats().summaryFor(chat);
        registry.check(before.isOk() && !before.value().blocked, "a contact's chat is not blocked");

        registry.check(d.core->blocks().block(alice).isOk(), "a contact can be blocked");

        auto summary = d.core->chats().summaryFor(chat);
        registry.check(summary.isOk() && summary.value().blocked,
                       "the chat row says it is blocked, so the screen can hide the composer");
        auto rows = d.core->chats().summaries();
        registry.check(rows.isOk() && rows.value().size() == 1 && rows.value().front().blocked,
                       "the chat list row says so too");

        auto sent = d.core->chats().send(chat, "still there?", ClientMessageId{"c1"});
        registry.check(sent.isError() && sent.result().code() == ErrorCode::PermissionDenied,
                       "sending to a blocked contact is refused");

        const Byte jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00};
        auto photo = d.core->chats().sendMedia(chat, "image/jpeg", "", ByteSpan{jpeg, sizeof jpeg},
                                               ClientMessageId{"c2"});
        registry.check(photo.isError() && photo.result().code() == ErrorCode::PermissionDenied,
                       "a photo cannot slip past a block that stops text");

        auto history = d.core->chats().history(chat, Cursor{});
        registry.check(history.isOk() && history.value().messages.empty(),
                       "a refused send leaves no bubble behind");

        registry.check(d.core->blocks().unblock(alice).isOk(), "the contact can be unblocked");
        auto resumed = d.core->chats().send(chat, "sorry", ClientMessageId{"c3"});
        registry.check(resumed.isOk(), "after unblocking, the chat works again");
        auto unblocked = d.core->chats().summaryFor(chat);
        registry.check(unblocked.isOk() && !unblocked.value().blocked,
                       "and the chat row is no longer marked blocked");
    }

    // ── resending a failed message ───────────────────────────────────────────

    registry.section("Retry — only a failed message of ours");
    {
        Device d(freshDir());
        registry.check(d.signUp("vinay"), "device signs up");
        const ChatId chat = d.befriend(alice, "alice");

        const MessageId failed =
            d.storeMessage(chat, Direction::Outgoing, DeliveryStatus::Failed, "f1");
        d.events.statuses.clear();

        registry.check(d.core->chats().retry(failed).isOk(), "a failed message can be resent");
        registry.check(d.statusOf(failed) != DeliveryStatus::Failed,
                       "a resent message is no longer marked failed");
        registry.check(!d.events.statuses.empty() && d.events.statuses.front().first == failed
                           && d.events.statuses.front().second == DeliveryStatus::Queued,
                       "the chat screen is told it is queued again");

        const MessageId queued =
            d.storeMessage(chat, Direction::Outgoing, DeliveryStatus::Queued, "q1");
        auto q = d.core->chats().retry(queued);
        registry.check(q.isError() && q.code() == ErrorCode::PreconditionFailed,
                       "a message still on its way cannot be resent on top of itself");

        const MessageId delivered =
            d.storeMessage(chat, Direction::Outgoing, DeliveryStatus::Delivered, "d1");
        auto dl = d.core->chats().retry(delivered);
        registry.check(dl.isError() && d.statusOf(delivered) == DeliveryStatus::Delivered,
                       "a delivered message is refused and stays delivered");

        const MessageId inbound =
            d.storeMessage(chat, Direction::Incoming, DeliveryStatus::Failed, "i1");
        auto in = d.core->chats().retry(inbound);
        registry.check(in.isError() && in.code() == ErrorCode::InvalidArgument,
                       "someone else's message is never sent back out");

        auto missing = d.core->chats().retry(MessageId{"no-such-message"});
        registry.check(missing.isError() && missing.code() == ErrorCode::NotFound,
                       "an unknown message id is NotFound");

        const MessageId failedAgain =
            d.storeMessage(chat, Direction::Outgoing, DeliveryStatus::Failed, "f2");
        d.core->blocks().block(alice);
        auto blocked = d.core->chats().retry(failedAgain);
        registry.check(blocked.isError() && blocked.code() == ErrorCode::PermissionDenied,
                       "a resend to a blocked contact is refused");
        registry.check(d.statusOf(failedAgain) == DeliveryStatus::Failed,
                       "and the message stays failed rather than silently queueing");
    }

    // ── editing the account while the radio is up ────────────────────────────

    registry.section("Account changes reach the radio immediately");
    {
        Device d(freshDir());
        registry.check(d.signUp("vinay"), "device signs up");
        registry.check(d.radio->advertising, "a new account advertises");

        d.clock.runPending();
        registry.check(lastAnnouncedNickname(*d.radio) == std::optional<std::string>{"vinay"},
                       "the first announce carries the sign-up name");

        UpdateAccountRequest rename;
        rename.username = "vinay_k";
        auto renamed = d.core->updateAccount(rename);
        registry.check(renamed.isOk() && renamed.value().username == "vinay_k",
                       "the username can be changed");

        d.clock.runPending();
        registry.check(lastAnnouncedNickname(*d.radio) == std::optional<std::string>{"vinay_k"},
                       "the very next announce carries the new name — no restart needed");

        UpdateAccountRequest bad;
        bad.username = "Not Valid!";
        auto refused = d.core->updateAccount(bad);
        registry.check(refused.isError() && refused.result().code() == ErrorCode::InvalidArgument,
                       "an invalid username is refused");
        d.clock.runPending();
        registry.check(lastAnnouncedNickname(*d.radio) == std::optional<std::string>{"vinay_k"},
                       "and the radio keeps the last good name");

        UpdateAccountRequest hide;
        hide.discoverable = false;
        registry.check(d.core->updateAccount(hide).isOk(), "discoverability can be switched off");
        registry.check(!d.radio->advertising, "hiding stops advertising straight away");

        const std::size_t sentBefore = d.radio->broadcasts.size();
        d.clock.runPending();
        registry.check(d.radio->broadcasts.size() == sentBefore,
                       "a hidden device broadcasts no announce");

        UpdateAccountRequest show;
        show.discoverable = true;
        registry.check(d.core->updateAccount(show).isOk(), "discoverability can be switched on");
        registry.check(d.radio->advertising, "showing again resumes advertising");
        d.clock.runPending();
        registry.check(d.radio->broadcasts.size() > sentBefore,
                       "and announces resume on the next tick");

        UpdateAccountRequest nobody;
        nobody.acceptFrom = AcceptFrom::NoOne;
        auto closed = d.core->updateAccount(nobody);
        registry.check(closed.isOk() && closed.value().acceptFrom == AcceptFrom::NoOne,
                       "accept-from can be set to no one");
        d.receiveRequest(stranger, "stranger");
        registry.check(d.core->requests().inbound().value().empty(),
                       "and then a stranger's request is dropped");

        UpdateAccountRequest everyone;
        everyone.acceptFrom = AcceptFrom::Everyone;
        d.core->updateAccount(everyone);
        d.receiveRequest(stranger, "stranger");
        registry.check(d.core->requests().inbound().value().size() == 1,
                       "switching back to everyone lets requests in again");
    }

    registry.section("Account changes before the radio is up");
    {
        Device d(freshDir());
        UpdateAccountRequest rename;
        rename.username = "early";
        auto none = d.core->updateAccount(rename);
        registry.check(none.isError() && none.result().code() == ErrorCode::NotFound,
                       "there is nothing to update before sign-up");
    }

    // ── notifications ────────────────────────────────────────────────────────

    registry.section("Notifications — what the shade says");
    {
        Device d(freshDir());
        registry.check(d.signUp("vinay"), "device signs up");
        const ChatId chat = d.befriend(alice, "alice");
        const std::string aliceHandle = "@alice\xC2\xB7" + alice.shortLabel();

        auto inbound = [&](const std::string& body, const std::string& mediaId = "") {
            Message m;
            m.chatId    = chat;
            m.direction = Direction::Incoming;
            m.body      = body;
            m.mediaId   = mediaId;
            return m;
        };

        d.notifier.onMessageReceived(inbound("lunch?"));
        registry.check(d.shade.shown.size() == 1 && d.shade.shown[0].title == aliceHandle
                           && d.shade.shown[0].text == "lunch?"
                           && d.shade.shown[0].kind == notify::NoticeKind::Message,
                       "a message is titled with the sender's handle and shows its text");

        // Their live nickname changes; the pinned name must not.
        Peer renamed;
        renamed.peerId     = alice;
        renamed.nickname   = "bank_support";
        renamed.lastSeenAt = d.clock.now();
        d.store.upsertPeer(renamed);
        d.notifier.onMessageReceived(inbound("hi"));
        registry.check(d.shade.shown.back().title == aliceHandle,
                       "the title is the PINNED name, not what they announce themselves as now");

        d.notifier.onMessageReceived(inbound("", "media_1"));
        registry.check(d.shade.shown.back().text == "Photo", "an uncaptioned photo says Photo");

        Message mine = inbound("my own words");
        mine.direction = Direction::Outgoing;
        const std::size_t before = d.shade.shown.size();
        d.notifier.onMessageReceived(mine);
        Message stray = inbound("from nowhere");
        stray.chatId = ChatId{"no-such-chat"};
        d.notifier.onMessageReceived(stray);
        registry.check(d.shade.shown.size() == before,
                       "nothing is shown for our own message, or for a chat that does not exist");

        Request quiet;
        quiet.peerId    = stranger;
        quiet.direction = Direction::Incoming;
        quiet.nickname  = "";
        d.notifier.onRequestReceived(quiet);
        registry.check(d.shade.shown.back().title == "New chat request"
                           && d.shade.shown.back().text == "@" + stranger.shortLabel() + " wants to chat",
                       "a request with no name or greeting still says who, by fingerprint");

        const std::string longText(1000, 'a');
        d.notifier.onMessageReceived(inbound(longText));
        registry.check(d.shade.shown.back().text.size() <= notify::kMaxNoticeTextBytes,
                       "a long message is clipped for the shade");
    }

    registry.section("Notifications — clipping never splits a character");
    {
        // 'é' is two bytes and '😀' four. A naive byte cut through either
        // produces invalid UTF-8, which JNI's string conversion rejects.
        std::string accents;
        for (int i = 0; i < 200; ++i) accents += "\xC3\xA9";
        const std::string clipped = notify::clipForNotice(accents, 101);
        const std::string ellipsis = "\xE2\x80\xA6";
        const std::string kept = clipped.substr(0, clipped.size() - ellipsis.size());
        registry.check(clipped.size() <= 101 && kept.size() % 2 == 0
                           && clipped.compare(clipped.size() - 3, 3, ellipsis) == 0,
                       "two-byte characters are cut whole, and an ellipsis marks the cut");

        std::string emoji;
        for (int i = 0; i < 100; ++i) emoji += "\xF0\x9F\x98\x80";
        const std::string cutEmoji = notify::clipForNotice(emoji, 50);
        const std::string keptEmoji = cutEmoji.substr(0, cutEmoji.size() - 3);
        registry.check(cutEmoji.size() <= 50 && keptEmoji.size() % 4 == 0,
                       "four-byte emoji are cut whole");

        registry.check(notify::clipForNotice("short", 50) == "short",
                       "text that fits is left exactly as it was");
    }

    // ── transactions ─────────────────────────────────────────────────────────

    registry.section("Transactions — all or nothing");
    {
        Device d(freshDir());
        const PeerId p1 = peerFrom("1111111111111111");
        const PeerId p2 = peerFrom("2222222222222222");
        const PeerId p3 = peerFrom("3333333333333333");
        auto contactFor = [&](const PeerId& id) {
            Contact c;
            c.peerId          = id;
            c.displayName     = "x";
            c.pinnedStaticKey = filledKey(0x01);
            return c;
        };

        registry.check(d.store.runInTransaction([&] { return d.store.addContact(contactFor(p1)); })
                               .isOk()
                           && hasContact(d, p1),
                       "a unit that succeeds is committed");

        const Result failed = d.store.runInTransaction([&]() -> Result {
            d.store.addContact(contactFor(p2));
            return Result::error(ErrorCode::StorageFailure, "second step failed");
        });
        registry.check(failed.isError() && !hasContact(d, p2),
                       "a unit that fails leaves nothing of its first step behind");

        const Result nested = d.store.runInTransaction([&]() -> Result {
            d.store.addContact(contactFor(p2));
            d.store.runInTransaction([&]() -> Result {
                d.store.addContact(contactFor(p3));
                return Result::error(ErrorCode::StorageFailure, "inner failed");
            });
            return Result::ok();  // the outer step does not notice...
        });
        registry.check(nested.isError(), "...but an inner failure still fails the outer unit");
        registry.check(!hasContact(d, p2) && !hasContact(d, p3),
                       "and every write in both is rolled back");

        registry.check(d.store.runInTransaction([&] { return d.store.addContact(contactFor(p2)); })
                               .isOk()
                           && hasContact(d, p2),
                       "a failed unit does not poison the next one");
    }

    // ── two phones in range ──────────────────────────────────────────────────

    registry.section("Two phones — request, accept, and padded messages on the air");
    {
        Air air;
        Device a(freshDir(), &air);
        Device b(freshDir(), &air);
        registry.check(a.signUp("asha") && b.signUp("bala"), "both phones sign up");

        settle(air, a, b);
        auto seen = a.core->discovery().reachablePeers();
        bool aSeesB = false;
        if (seen.isOk()) {
            for (const Peer& p : seen.value()) aSeesB = aSeesB || p.peerId == b.self();
        }
        registry.check(aSeesB, "each sees the other from signed announces");

        registry.check(a.core->requests().send(b.self(), "hi!").isOk(), "asha sends a request");
        settle(air, a, b);
        auto inbox = b.core->requests().inbound();
        registry.check(inbox.isOk() && inbox.value().size() == 1
                           && inbox.value().front().nickname == "asha",
                       "bala receives it over an encrypted session");
        if (!inbox.isOk() || inbox.value().empty()) return;

        const std::string ashaHandle = "@asha\xC2\xB7" + a.self().shortLabel();
        registry.check(b.shade.shown.size() == 1
                           && b.shade.shown.back().kind == notify::NoticeKind::Request
                           && b.shade.shown.back().text == ashaHandle + ": hi!"
                           && b.shade.shown.back().route == "requests",
                       "bala's phone notifies her of the request, and a tap opens Requests");
        b.shade.shown.clear();

        auto chatB = b.core->requests().accept(inbox.value().front().id);
        registry.check(chatB.isOk(), "bala accepts");
        settle(air, a, b);
        registry.check(a.events.accepted.size() == 1 && hasContact(a, b.self())
                           && hasContact(b, a.self()),
                       "both sides now hold the other as a contact");
        if (a.events.accepted.empty()) return;
        const ChatId chatA = a.events.accepted.back().id;

        std::size_t mark = air.heard.size();
        auto yes = a.core->chats().send(chatA, "y", ClientMessageId{"m-short"});
        settle(air, a, b);
        const auto shortSizes = encryptedSizesFrom(air, a.self(), mark);

        mark = air.heard.size();
        const std::string paragraph(200, 'p');
        a.core->chats().send(chatA, paragraph, ClientMessageId{"m-long"});
        settle(air, a, b);
        const auto longSizes = encryptedSizesFrom(air, a.self(), mark);

        registry.check(receivedBody(b, "y") && receivedBody(b, paragraph),
                       "both messages arrive with their exact text, padding stripped");
        registry.check(b.shade.shown.size() == 2 && b.shade.shown[0].title == ashaHandle
                           && b.shade.shown[0].text == "y"
                           && b.shade.shown[0].route == "chats/" + chatB.value().id.value()
                           && b.shade.shown[0].thread == chatB.value().id.value(),
                       "each message notifies bala, named by the contact she accepted, "
                       "and a tap opens that chat");
        registry.check(a.shade.shown.empty(),
                       "asha is never notified of her own messages");
        registry.check(shortSizes.size() == 1 && longSizes.size() == 1,
                       "each message is one encrypted packet on the air");
        registry.check(!shortSizes.empty() && !longSizes.empty() && shortSizes[0] == longSizes[0],
                       "a one-letter reply and a 200-letter one are the same size to a listener");
        registry.check(!shortSizes.empty() && shortSizes[0] == 256 + 16,
                       "that size is the 256-byte bucket plus the 16-byte tag");
        registry.check(yes.isOk() && a.statusOf(yes.value().id) == DeliveryStatus::Delivered,
                       "and the ack comes back, so the sender sees Delivered");

        // A photo: well past the largest bucket and past the 480-byte frame,
        // so it is padded, encrypted, fragmented and reassembled.
        ByteBuffer photo(20000);
        for (std::size_t i = 0; i < photo.size(); ++i) photo[i] = static_cast<Byte>(i * 31 + 7);
        photo[0] = 0xFF; photo[1] = 0xD8; photo[2] = 0xFF;
        auto sent = a.core->chats().sendMedia(chatA, "image/jpeg", "sunset", ByteSpan{photo},
                                              ClientMessageId{"p-1"});
        settle(air, a, b, 12);
        bool photoArrived = false;
        for (const Message& m : b.events.received) {
            if (!m.isMedia()) continue;
            auto bytes = b.store.getMedia(m.mediaId);
            photoArrived = bytes.isOk() && bytes.value() == photo && m.body == "sunset";
        }
        registry.check(sent.isOk() && photoArrived,
                       "a photo crosses in 480-byte fragments and arrives byte for byte");
        registry.check(!b.shade.shown.empty() && b.shade.shown.back().text == "Photo \xC2\xB7 sunset",
                       "a photo notifies as a photo, with its caption");
    }

    registry.section("Two phones — an accept that fails half way is undone");
    {
        Air air;
        Device a(freshDir(), &air);
        Device b(freshDir(), &air);
        a.signUp("asha");
        b.signUp("bala");
        settle(air, a, b);
        a.core->requests().send(b.self(), "hi!");
        settle(air, a, b);
        auto inbox = b.core->requests().inbound();
        registry.check(inbox.isOk() && inbox.value().size() == 1, "the request arrives");
        if (!inbox.isOk() || inbox.value().empty()) return;
        const RequestId id = inbox.value().front().id;

        b.flaky.failCreateChat = true;  // the disk fails after the contact is written
        registry.check(b.core->requests().accept(id).isError(), "the accept reports the failure");
        registry.check(!hasContact(b, a.self()),
                       "the contact written before the failure is rolled back");
        registry.check(b.core->chats().summaries().value().empty(), "no chat is left behind");
        registry.check(b.core->requests().inbound().value().size() == 1,
                       "the request is still waiting, so it can be accepted again");
        settle(air, a, b);
        registry.check(a.events.accepted.empty(),
                       "nothing was sent to the requester for an accept that did not happen");

        b.flaky.failCreateChat = false;
        registry.check(b.core->requests().accept(id).isOk(), "accepting again succeeds");
        settle(air, a, b);
        registry.check(a.events.accepted.size() == 1 && hasContact(a, b.self()),
                       "and the requester completes the pair");
    }

    registry.section("Two phones — the requester's side fails, and asking again repairs it");
    {
        Air air;
        Device a(freshDir(), &air);
        Device b(freshDir(), &air);
        a.signUp("asha");
        b.signUp("bala");
        settle(air, a, b);
        a.core->requests().send(b.self(), "hi!");
        settle(air, a, b);
        auto inbox = b.core->requests().inbound();
        if (!inbox.isOk() || inbox.value().empty()) {
            registry.check(false, "the request arrives");
            return;
        }

        a.flaky.failCreateChat = true;
        registry.check(b.core->requests().accept(inbox.value().front().id).isOk(),
                       "bala accepts");
        settle(air, a, b);
        registry.check(!hasContact(a, b.self()) && a.events.accepted.empty(),
                       "asha's device fails to record it, and rolls back cleanly");

        a.flaky.failCreateChat = false;
        a.core->requests().send(b.self(), "hi again");
        settle(air, a, b);
        registry.check(hasContact(a, b.self()) && a.events.accepted.size() == 1,
                       "asking again makes bala re-send the accept, and the pair completes");
        registry.check(b.core->chats().summaries().value().size() == 1,
                       "bala still has exactly one chat with her");
        registry.check(b.core->requests().inbound().value().empty(),
                       "and no duplicate request appears in bala's Requests tab");
    }
}

}  // namespace anytime::test
