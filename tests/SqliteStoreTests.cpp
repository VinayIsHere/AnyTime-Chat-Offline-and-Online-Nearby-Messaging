// tests/SqliteStoreTests.cpp
//
// The repository layer against a real SQLite file in a temporary directory.
//
// These assert the properties whose absence would not otherwise be visible until
// a user lost data: that rows survive a close and reopen, that the block list
// works for a peer who is not a contact, that paging is stable when two messages
// share a timestamp, and that a message body is genuinely unreadable in the file
// on disk.

#include "tests/TestHarness.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <memory>

#include "adapters/persistence/sqlite/MonocypherDataCipher.h"
#include "adapters/persistence/sqlite/SqliteStore.h"
#include "core/ports/IClock.h"

namespace anytime::test {

namespace {

/// Time under the test's control. The outbox ladder and the peer window are both
/// time-driven, and a test must exercise them without sleeping.
class StubClock final : public IClock {
public:
    Timestamp now() const override { return now_; }
    Timestamp monotonic() const override { return now_; }

    TimerId scheduleOnce(Timestamp, std::function<void()>) override { return kInvalidTimer; }
    TimerId scheduleRepeating(Timestamp, std::function<void()>) override { return kInvalidTimer; }
    void    cancel(TimerId) override {}

    void advance(Timestamp millis) { now_ += millis; }

    Timestamp now_ = 1'700'000'000'000;
};

PeerId peerFrom(const char* hex)
{
    return PeerId::fromHex(hex).value_or(PeerId{});
}

PublicKey filledKey(Byte value)
{
    PublicKey key{};
    key.fill(value);
    return key;
}

/// Reads the database and its WAL sidecar as raw bytes, so a test can prove a
/// plaintext string is not sitting in either of them.
QByteArray rawDatabaseBytes(const QString& path)
{
    QByteArray all;
    for (const QString& candidate : {path, path + QStringLiteral("-wal")}) {
        QFile file(candidate);
        if (file.open(QIODevice::ReadOnly)) all += file.readAll();
    }
    return all;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

void runSqliteStoreTests(Registry& registry)
{
    registry.section("SqliteStore");

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        registry.check(false, "temporary directory for the database");
        return;
    }

    const QString dbPath  = tempDir.path() + QStringLiteral("/anytime.db");
    const QString keyPath = tempDir.path() + QStringLiteral("/data.key");

    persistence::MonocypherDataCipher cipher(keyPath);
    registry.check(cipher.loadOrCreate().isOk(), "data key is created on first run");

    StubClock clock;

    // ── the cipher itself ────────────────────────────────────────────────────

    {
        const std::string plaintext = "the quick brown fox";
        ByteBuffer sealed;
        const auto* p = reinterpret_cast<const Byte*>(plaintext.data());
        registry.check(cipher.seal(ByteSpan{p, plaintext.size()}, sealed).isOk(),
                       "cipher seals a value");
        registry.check(sealed.size() == plaintext.size() + 40,
                       "sealed record carries a 24-byte nonce and a 16-byte tag");

        ByteBuffer opened;
        registry.check(cipher.open(ByteSpan{sealed}, opened).isOk(), "cipher opens what it sealed");
        registry.check(std::string(opened.begin(), opened.end()) == plaintext,
                       "round-trip preserves the plaintext");

        // A tampered record must fail authentication rather than yield garbage.
        ByteBuffer tampered = sealed;
        tampered[tampered.size() - 1] ^= 0x01;
        ByteBuffer ignored;
        registry.check(cipher.open(ByteSpan{tampered}, ignored).isError(),
                       "a tampered record is rejected, not silently accepted");
    }

    const PeerId alice = peerFrom("0011223344556677");
    const PeerId mallory = peerFrom("8899aabbccddeeff");

    // ── schema, account, and durability across a reopen ──────────────────────

    {
        persistence::SqliteStore store(dbPath, cipher, clock);
        registry.check(store.open().isOk(), "database opens");
        // Bumped with each migration. The media suite below asserts the same
        // number reached an already-open database, which is what proves the
        // migration ran rather than the schema merely being created at v2.
        registry.check(store.schemaVersion() == 3, "schema is stamped at the current version");

        auto empty = store.load();
        registry.check(empty.isOk() && !empty.value().has_value(),
                       "no account before one is saved");

        Account account;
        account.peerId           = alice;
        account.username         = "vinay";
        account.dateOfBirth      = "1998-04-12";
        account.gender           = Gender::Male;
        account.discoverable     = true;
        account.acceptFrom       = AcceptFrom::Everyone;
        account.createdAt        = clock.now();
        account.staticPublicKey  = filledKey(0x11);
        account.signingPublicKey = filledKey(0x22);

        registry.check(store.save(account).isOk(), "account saves");

        // Saving twice must update the single row rather than fail on the PK.
        account.username = "vinay2";
        registry.check(store.save(account).isOk(), "account save is idempotent");

        store.close();
    }

    {
        persistence::SqliteStore store(dbPath, cipher, clock);
        registry.check(store.open().isOk(), "database reopens");

        auto loaded = store.load();
        registry.check(loaded.isOk() && loaded.value().has_value(),
                       "account survives a close and reopen");
        registry.check(loaded.value()->username == "vinay2", "the updated username persisted");
        registry.check(loaded.value()->peerId == alice, "peer id round-trips");
        registry.check(loaded.value()->staticPublicKey == filledKey(0x11),
                       "public key blob round-trips");
        store.close();
    }

    // ── contacts, and the block list for a NON-contact ───────────────────────

    {
        persistence::SqliteStore store(dbPath, cipher, clock);
        registry.check(store.open().isOk(), "database opens for the contact suite");

        Contact contact;
        contact.peerId          = alice;
        contact.pinnedStaticKey = filledKey(0x33);
        contact.displayName     = "vinay";
        contact.createdAt       = clock.now();
        registry.check(store.addContact(contact).isOk(), "contact is added");

        auto found = store.findContact(alice);
        registry.check(found.isOk() && found.value().has_value(), "contact is found");
        registry.check(found.value()->displayName == "vinay", "display name round-trips");
        registry.check(found.value()->pinnedStaticKey == filledKey(0x33),
                       "the pinned static key round-trips");
        registry.check(!found.value()->isBlocked(), "a new contact is not blocked");

        // THE CASE A contact.blocked_at COLUMN WOULD MISS: declining with block
        // blocks someone who never became a contact.
        registry.check(!store.isBlocked(mallory), "an unknown peer is not blocked");
        registry.check(store.setBlocked(mallory, true).isOk(), "a non-contact peer can be blocked");
        registry.check(store.isBlocked(mallory), "the block on a non-contact sticks");

        registry.check(store.setBlocked(alice, true).isOk(), "a contact can be blocked");
        auto blocked = store.findContact(alice);
        registry.check(blocked.isOk() && blocked.value().has_value()
                           && blocked.value()->isBlocked(),
                       "contact reads back as blocked");

        registry.check(store.setBlocked(alice, false).isOk(), "a contact can be unblocked");
        registry.check(!store.isBlocked(alice), "unblocking sticks");

        auto contacts = store.listContacts();
        registry.check(contacts.isOk() && contacts.value().size() == 1, "contact list has one row");

        store.close();
    }

    // ── requests ─────────────────────────────────────────────────────────────

    {
        persistence::SqliteStore store(dbPath, cipher, clock);
        registry.check(store.open().isOk(), "database opens for the request suite");

        Request request;
        request.id        = RequestId{"req_1"};
        request.peerId    = mallory;
        request.direction = Direction::Incoming;
        request.nickname  = "arjun";
        request.greeting  = "hello";
        request.state     = RequestState::Pending;
        request.createdAt = clock.now();
        registry.check(store.saveRequest(request).isOk(), "request saves");

        auto pending = store.listRequests(Direction::Incoming, RequestState::Pending);
        registry.check(pending.isOk() && pending.value().size() == 1, "pending inbound request listed");
        registry.check(pending.value().front().greeting == "hello", "greeting round-trips");

        // The filter must actually discriminate, not just return everything.
        auto outgoing = store.listRequests(Direction::Outgoing, RequestState::Pending);
        registry.check(outgoing.isOk() && outgoing.value().empty(),
                       "the direction filter excludes the inbound row");

        registry.check(store.setRequestState(RequestId{"req_1"}, RequestState::Accepted).isOk(),
                       "request state updates");
        auto afterAccept = store.listRequests(Direction::Incoming, RequestState::Pending);
        registry.check(afterAccept.isOk() && afterAccept.value().empty(),
                       "an accepted request leaves the pending list");

        registry.check(store.setRequestState(RequestId{"nope"}, RequestState::Declined).isError(),
                       "updating a missing request is an error, not a silent no-op");

        auto count = store.countInboundRequestsSince(mallory, clock.now() - 1000);
        registry.check(count.isOk() && count.value() == 1, "inbound requests are counted for rate limiting");

        store.close();
    }

    // ── chats and messages ───────────────────────────────────────────────────

    const std::string secretBody = "sekret-plaintext-body-do-not-leak";

    {
        persistence::SqliteStore store(dbPath, cipher, clock);
        registry.check(store.open().isOk(), "database opens for the message suite");

        auto chat = store.createChat(alice);
        registry.check(chat.isOk(), "chat is created");

        auto again = store.createChat(alice);
        registry.check(again.isOk() && again.value().id == chat.value().id,
                       "createChat is one-per-peer, not a second chat");

        const ChatId chatId = chat.value().id;

        // Three messages sharing ONE timestamp — the case that breaks paging
        // built on created_at.
        for (int i = 0; i < 3; ++i) {
            Message message;
            message.chatId          = chatId;
            message.clientMessageId = ClientMessageId{"cmid_" + std::to_string(i)};
            message.direction       = Direction::Outgoing;
            message.body            = secretBody + std::to_string(i);
            message.status          = DeliveryStatus::Queued;
            message.createdAt       = clock.now();  // identical on purpose
            registry.check(store.append(message).isOk(), "message " + std::to_string(i) + " appends");
        }

        auto firstPage = store.page(chatId, Cursor{}, 2);
        registry.check(firstPage.isOk() && firstPage.value().messages.size() == 2,
                       "first page honours the limit");
        registry.check(firstPage.value().hasMore, "first page reports more available");
        registry.check(firstPage.value().messages.front().body == secretBody + "2",
                       "history is newest first");

        auto secondPage = store.page(chatId, firstPage.value().nextCursor, 2);
        registry.check(secondPage.isOk() && secondPage.value().messages.size() == 1,
                       "the cursor returns the remaining row, with no repeat or skip");
        registry.check(secondPage.value().messages.front().body == secretBody + "0",
                       "the last row is the oldest message");
        registry.check(!secondPage.value().hasMore, "the final page reports no more");

        auto byClient = store.findByClientId(ClientMessageId{"cmid_1"});
        registry.check(byClient.isOk() && byClient.value().has_value(),
                       "a message is findable by client id (send idempotency)");
        registry.check(byClient.value()->body == secretBody + "1", "the body decrypts correctly");

        const MessageId id = byClient.value()->id;
        registry.check(store.setStatus(id, DeliveryStatus::Delivered).isOk(), "status updates");
        auto reread = store.findMessage(id);
        registry.check(reread.isOk() && reread.value().has_value()
                           && reread.value()->status == DeliveryStatus::Delivered,
                       "the new status persisted");
        registry.check(reread.value()->deliveredAt != kNoTimestamp,
                       "delivered_at is stamped on the transition");

        registry.check(store.setUnreadCount(chatId, 3).isOk(), "unread count updates");
        auto chats = store.listChats();
        registry.check(chats.isOk() && chats.value().size() == 1
                           && chats.value().front().unreadCount == 3,
                       "the chat list carries the unread count");

        store.close();
    }

    // ── media blobs ──────────────────────────────────────────────────────────

    // Recognisable bytes, so the at-rest scan below can prove they are not
    // sitting in the file in the clear.
    const std::string imageMarker = "JPEGDATA-not-encrypted-marker";
    ByteBuffer image(imageMarker.begin(), imageMarker.end());
    image.resize(4096, 0x7E);  // pad out to something photo-shaped

    {
        persistence::SqliteStore store(dbPath, cipher, clock);
        registry.check(store.open().isOk(), "database opens for the media suite");
        registry.check(store.schemaVersion() == 3, "every migration ran on this database");

        registry.check(store.putMedia("media_1", ByteSpan{image}).isOk(), "image is stored");

        auto fetched = store.getMedia("media_1");
        registry.check(fetched.isOk(), "image is retrieved");
        registry.check(fetched.value() == image, "image bytes survive the round trip exactly");

        auto missing = store.getMedia("nope");
        registry.check(missing.isError(), "an unknown media id is an error, not empty bytes");

        registry.check(store.putMedia("", ByteSpan{image}).isError(), "an empty media id is refused");
        registry.check(store.putMedia("media_2", ByteSpan{}).isError(), "an empty image is refused");

        // Re-receiving the same image must replace it, not fail on the primary key.
        registry.check(store.putMedia("media_1", ByteSpan{image}).isOk(),
                       "re-storing the same image id replaces rather than fails");

        // A message referencing the image keeps only the reference.
        auto chat = store.findChatByPeer(alice);
        registry.check(chat.isOk() && chat.value().has_value(), "chat still exists for media test");

        Message photo;
        photo.chatId          = chat.value()->id;
        photo.clientMessageId = ClientMessageId{"cmid_media"};
        photo.direction       = Direction::Outgoing;
        photo.body            = "look at this";  // caption
        photo.status          = DeliveryStatus::Queued;
        photo.createdAt       = clock.now();
        photo.mediaId         = "media_1";
        photo.mediaMimeType   = "image/jpeg";
        photo.mediaBytes      = image.size();

        auto stored = store.append(photo);
        registry.check(stored.isOk(), "a media message appends");

        auto readBack = store.findMessage(stored.value());
        registry.check(readBack.isOk() && readBack.value().has_value(), "media message is found");
        registry.check(readBack.value()->isMedia(), "message reports itself as media");
        registry.check(readBack.value()->mediaId == "media_1", "media id round-trips");
        registry.check(readBack.value()->mediaMimeType == "image/jpeg", "mime type round-trips");
        registry.check(readBack.value()->mediaBytes == image.size(), "byte count round-trips");
        registry.check(readBack.value()->body == "look at this", "caption round-trips");

        // A plain text message must still report itself as text.
        auto text = store.findByClientId(ClientMessageId{"cmid_1"});
        registry.check(text.isOk() && text.value().has_value() && !text.value()->isMedia(),
                       "a text message is not mistaken for media");

        store.close();
    }

    // ── at rest: the body must not be readable in the file ───────────────────

    {
        const QByteArray raw = rawDatabaseBytes(dbPath);
        registry.check(!raw.isEmpty(), "the database file has content to inspect");
        registry.check(!raw.contains(QByteArray::fromStdString(secretBody)),
                       "no message body appears as plaintext in the database file");
        registry.check(!raw.contains(QByteArray::fromStdString(imageMarker)),
                       "no image bytes appear as plaintext in the database file");
        // The control: a value stored unencrypted IS findable, which proves the
        // check above is actually looking at real data.
        registry.check(raw.contains(QByteArray("vinay2")),
                       "an unencrypted column is findable, so the scan is meaningful");
    }

    // ── outbox ───────────────────────────────────────────────────────────────

    {
        persistence::SqliteStore store(dbPath, cipher, clock);
        registry.check(store.open().isOk(), "database opens for the outbox suite");

        OutboxEntry entry;
        entry.messageId     = MessageId{"msg_1"};
        entry.peerId        = alice;
        entry.payload       = ByteBuffer{0xDE, 0xAD, 0xBE, 0xEF};
        entry.attempts      = 0;
        entry.nextAttemptAt = clock.now();
        entry.expiresAt     = clock.now() + kOutboxTtlMillis;
        // Bulk, so the assertion below proves the class is not silently reset to
        // Text — which is exactly what happened before it was persisted, and
        // would have sent every restarted photo over the wrong transport.
        entry.payloadClass  = PayloadClass::Bulk;
        registry.check(store.enqueue(entry).isOk(), "outbox entry is queued");

        auto due = store.dueFor(alice, clock.now());
        registry.check(due.isOk() && due.value().size() == 1, "the entry is due now");
        registry.check(due.value().front().payload == ByteBuffer({0xDE, 0xAD, 0xBE, 0xEF}),
                       "the encrypted payload round-trips");
        registry.check(due.value().front().payloadClass == PayloadClass::Bulk,
                       "the payload class survives storage, so a queued photo stays bulk");

        auto notDue = store.dueFor(alice, clock.now() - 10'000);
        registry.check(notDue.isOk() && notDue.value().empty(),
                       "an entry scheduled later is not returned");

        auto peers = store.peersWithPending();
        registry.check(peers.isOk() && peers.value().size() == 1,
                       "the peer shows up as having pending mail");

        registry.check(store.recordAttempt(MessageId{"msg_1"}, clock.now() + 5000).isOk(),
                       "an attempt is recorded");
        auto afterAttempt = store.dueFor(alice, clock.now());
        registry.check(afterAttempt.isOk() && afterAttempt.value().empty(),
                       "recording an attempt pushes the entry out of the due window");

        // Past the TTL, the entry must be reaped and handed back so the caller
        // can flip the message to Failed.
        clock.advance(kOutboxTtlMillis + 1000);
        auto reaped = store.reapExpired(clock.now());
        registry.check(reaped.isOk() && reaped.value().size() == 1,
                       "an expired entry is reaped and returned");
        registry.check(store.peersWithPending().value().empty(),
                       "the reaped entry is gone from the outbox");

        store.close();
    }

    // ── panic wipe ───────────────────────────────────────────────────────────

    {
        persistence::SqliteStore store(dbPath, cipher, clock);
        registry.check(store.open().isOk(), "database opens for the wipe suite");
        registry.check(store.wipeAll().isOk(), "wipeAll succeeds");

        auto account = store.load();
        registry.check(account.isOk() && !account.value().has_value(),
                       "the account is gone after a wipe");
        auto chats = store.listChats();
        registry.check(chats.isOk() && chats.value().empty(), "chats are gone after a wipe");
        auto contacts = store.listContacts();
        registry.check(contacts.isOk() && contacts.value().empty(),
                       "contacts are gone after a wipe");
        store.close();
    }
}

}  // namespace anytime::test
