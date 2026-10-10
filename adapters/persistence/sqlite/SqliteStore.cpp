// adapters/persistence/sqlite/SqliteStore.cpp
//
// Every repository port over SQLite. Schema: docs/ARCHITECTURE.md section 12.
//
// Logs here carry ids, counts and error codes — never a message body, a greeting
// or key material (core/log/Log.h states the rule).
//
// THREE THINGS WORTH KNOWING BEFORE EDITING:
//
// 1. Ids are generated with MAX(seq)+1 rather than a sequence object. That is
//    safe because the core is single-threaded by design (core/AppCore.h) and
//    every repository call arrives on the core thread. If that ever stops being
//    true, this is one of the places that breaks.
//
// 2. History pages on `seq`, not `created_at`. Two messages can share a
//    millisecond, and a cursor over a non-unique column either repeats a row or
//    skips one. `seq` is the insertion order and is unique by construction.
//
// 3. The block list is its own table, NOT contact.blocked_at. Declining a
//    request with `block` blocks a peer who is usually not a contact at all
//    (RequestService::decline), so keying blocks on the contact row would
//    silently drop them. Contact::blockedAt is populated from that table on read.

#include "adapters/persistence/sqlite/SqliteStore.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <atomic>
#include <cstring>
#include <limits>

#include "core/log/Log.h"
#include "core/ports/IClock.h"

namespace anytime::persistence {

namespace {

/// Schema version written to PRAGMA user_version. Migrations are forward-only:
/// add a new `if (version < N)` block, never edit an existing one.
///
/// v2 added the media table and the message columns that reference it.
/// v3 added outbox.payload_class, so a queued photo still knows it is bulk
/// after a restart.
constexpr int kSchemaVersion = 3;

QString hexOf(const PeerId& id) { return QString::fromStdString(id.toHex()); }

QByteArray keyBlob(const PublicKey& key)
{
    return QByteArray(reinterpret_cast<const char*>(key.data()),
                      static_cast<int>(kPublicKeyBytes));
}

bool keyFromBlob(const QByteArray& blob, PublicKey& out)
{
    if (blob.size() != static_cast<int>(kPublicKeyBytes)) return false;
    std::memcpy(out.data(), blob.constData(), kPublicKeyBytes);
    return true;
}

ByteBuffer toBuffer(const QByteArray& blob)
{
    const auto* p = reinterpret_cast<const Byte*>(blob.constData());
    return ByteBuffer(p, p + blob.size());
}

QByteArray fromBuffer(const ByteBuffer& buffer)
{
    return QByteArray(reinterpret_cast<const char*>(buffer.data()),
                      static_cast<int>(buffer.size()));
}

Result failed(const QSqlQuery& query, const char* what)
{
    AT_LOG_ERROR("storage", "{}: {}", what, query.lastError().text().toStdString());
    return Result::error(ErrorCode::StorageFailure, what);
}

/// Each store instance needs its own Qt connection, or `--instance b` would
/// share a handle with instance A and write into the wrong database.
QString nextConnectionName()
{
    static std::atomic<unsigned> counter{0};
    return QStringLiteral("anytime_sqlite_%1").arg(counter.fetch_add(1));
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct SqliteStore::Impl {
    QString      path;
    QString      connection;
    IDataCipher& cipher;
    IClock&      clock;
    bool         opened = false;

    Impl(QString p, IDataCipher& c, IClock& k)
        : path(std::move(p)), connection(nextConnectionName()), cipher(c), clock(k)
    {
    }

    QSqlDatabase db() const { return QSqlDatabase::database(connection); }

    /// How many runInTransaction calls are currently open. Only the outermost
    /// begins and ends the SQLite transaction; inner ones join it.
    int transactionDepth = 0;

    /// Set when an inner call failed, so the outermost rolls back even if its
    /// own work then reports success.
    bool transactionFailed = false;

    QSqlQuery query() const { return QSqlQuery(db()); }

    bool exec(const char* sql)
    {
        QSqlQuery q(db());
        if (q.exec(QLatin1String(sql))) return true;
        AT_LOG_ERROR("storage", "statement failed: {}", q.lastError().text().toStdString());
        return false;
    }

    int userVersion()
    {
        QSqlQuery q(db());
        if (q.exec(QStringLiteral("PRAGMA user_version")) && q.next()) return q.value(0).toInt();
        return 0;
    }

    Result migrate();

    /// Encrypts on the way in. Returns false only when the data key is missing.
    bool seal(const std::string& plaintext, QByteArray& out)
    {
        ByteBuffer sealed;
        const auto* p = reinterpret_cast<const Byte*>(plaintext.data());
        if (cipher.seal(ByteSpan{p, plaintext.size()}, sealed).isError()) return false;
        out = fromBuffer(sealed);
        return true;
    }

    /// Decrypts on the way out. A row that fails authentication yields an empty
    /// string rather than aborting the whole query — one unreadable message must
    /// not make the entire chat list unopenable.
    std::string openText(const QByteArray& blob)
    {
        ByteBuffer plain;
        const auto* p = reinterpret_cast<const Byte*>(blob.constData());
        if (cipher.open(ByteSpan{p, static_cast<std::size_t>(blob.size())}, plain).isError()) {
            return std::string();
        }
        return std::string(plain.begin(), plain.end());
    }

    bool isBlockedInternal(const PeerId& peerId)
    {
        QSqlQuery q(db());
        q.prepare(QStringLiteral("SELECT 1 FROM blocked WHERE peer_id = ?"));
        q.addBindValue(hexOf(peerId));
        return q.exec() && q.next();
    }

    Outcome<qint64> nextSeq(const char* table);
};

Outcome<qint64> SqliteStore::Impl::nextSeq(const char* table)
{
    QSqlQuery q(db());
    const QString sql = QStringLiteral("SELECT COALESCE(MAX(seq), 0) + 1 FROM %1")
                            .arg(QLatin1String(table));
    if (!q.exec(sql) || !q.next()) return failed(q, "Could not allocate a row id.");
    return q.value(0).toLongLong();
}

// ── schema ───────────────────────────────────────────────────────────────────

Result SqliteStore::Impl::migrate()
{
    const int version = userVersion();
    if (version >= kSchemaVersion) return Result::ok();

    if (version == 0) {
        AT_LOG_INFO("storage", "creating schema v{}", kSchemaVersion);

        static const char* const kSchema[] = {
            // Exactly one account row, enforced by the CHECK rather than by
            // convention. Secret keys are NOT here: they live in the identity
            // store, which is the only component that ever holds them (AD-05).
            // Section 12's `static_sk_wrapped` columns predate that split.
            "CREATE TABLE IF NOT EXISTS account ("
            "  id            INTEGER PRIMARY KEY CHECK (id = 1),"
            "  peer_id       TEXT    NOT NULL,"
            "  username      TEXT    NOT NULL,"
            "  dob           TEXT    NOT NULL,"
            "  gender        INTEGER NOT NULL,"
            "  discoverable  INTEGER NOT NULL,"
            "  accept_from   INTEGER NOT NULL,"
            "  static_pk     BLOB,"
            "  signing_pk    BLOB,"
            "  created_at    INTEGER NOT NULL)",

            // Peers are re-learned from announces on every launch, so this table
            // is a cache, not a record. Kept in SQLite anyway so the Nearby list
            // survives a restart long enough to be useful.
            "CREATE TABLE IF NOT EXISTS peer ("
            "  peer_id              TEXT PRIMARY KEY,"
            "  static_pk            BLOB,"
            "  nickname             TEXT,"
            "  signal               INTEGER NOT NULL DEFAULT 3,"
            "  verified_announce_at INTEGER NOT NULL DEFAULT 0,"
            "  last_seen_at         INTEGER NOT NULL DEFAULT 0)",
            "CREATE INDEX IF NOT EXISTS idx_peer_last_seen ON peer(last_seen_at)",

            // pinned_static_pk is the trust anchor (section 08). Captured once
            // at accept time and never updated.
            "CREATE TABLE IF NOT EXISTS contact ("
            "  peer_id          TEXT PRIMARY KEY,"
            "  pinned_static_pk BLOB NOT NULL,"
            "  display_name     TEXT,"
            "  created_at       INTEGER NOT NULL DEFAULT 0)",

            // Keyed on fingerprint, never nickname, so a block survives the peer
            // renaming themselves. Separate from `contact` on purpose — see the
            // note at the top of this file.
            "CREATE TABLE IF NOT EXISTS blocked ("
            "  peer_id    TEXT PRIMARY KEY,"
            "  blocked_at INTEGER NOT NULL)",

            "CREATE TABLE IF NOT EXISTS request ("
            "  id         TEXT PRIMARY KEY,"
            "  peer_id    TEXT    NOT NULL,"
            "  direction  INTEGER NOT NULL,"
            "  nickname   TEXT,"
            "  greeting   TEXT,"
            "  state      INTEGER NOT NULL,"
            "  created_at INTEGER NOT NULL)",
            "CREATE INDEX IF NOT EXISTS idx_request_peer "
            "  ON request(peer_id, direction, state)",

            // One chat per peer, enforced by UNIQUE rather than by the caller
            // remembering to check.
            "CREATE TABLE IF NOT EXISTS chat ("
            "  id              TEXT PRIMARY KEY,"
            "  peer_id         TEXT    NOT NULL UNIQUE,"
            "  created_at      INTEGER NOT NULL,"
            "  last_message_at INTEGER NOT NULL DEFAULT 0,"
            "  unread          INTEGER NOT NULL DEFAULT 0)",

            // body_encrypted holds XChaCha20-Poly1305 ciphertext (section 10).
            // `seq` is the paging key; see the note at the top of this file.
            "CREATE TABLE IF NOT EXISTS message ("
            "  seq            INTEGER PRIMARY KEY,"
            "  id             TEXT    NOT NULL UNIQUE,"
            "  chat_id        TEXT    NOT NULL,"
            "  client_msg_id  TEXT,"
            "  direction      INTEGER NOT NULL,"
            "  body_encrypted BLOB    NOT NULL,"
            "  status         INTEGER NOT NULL,"
            "  created_at     INTEGER NOT NULL,"
            "  delivered_at   INTEGER NOT NULL DEFAULT 0,"
            "  read_at        INTEGER NOT NULL DEFAULT 0)",
            "CREATE INDEX IF NOT EXISTS idx_message_chat ON message(chat_id, seq DESC)",
            "CREATE INDEX IF NOT EXISTS idx_message_client ON message(client_msg_id)",

            // Durable pending sends — this is what makes a message survive the
            // app being killed and the peer being away for hours (section 09).
            "CREATE TABLE IF NOT EXISTS outbox ("
            "  message_id        TEXT PRIMARY KEY,"
            "  peer_id           TEXT    NOT NULL,"
            "  attempts          INTEGER NOT NULL DEFAULT 0,"
            "  next_attempt_at   INTEGER NOT NULL DEFAULT 0,"
            "  expires_at        INTEGER NOT NULL DEFAULT 0,"
            "  payload_encrypted BLOB)",
            "CREATE INDEX IF NOT EXISTS idx_outbox_peer ON outbox(peer_id, next_attempt_at)",
        };

        for (const char* statement : kSchema) {
            if (!exec(statement)) {
                return Result::error(ErrorCode::StorageFailure, "Could not create the schema.");
            }
        }
    }

    // ── v2: media ────────────────────────────────────────────────────────────
    //
    // A fresh database runs this straight after v1 rather than having the media
    // columns baked into the CREATE TABLE above. That is deliberate: it means
    // the migration path is exercised on every clean install, so it cannot rot
    // while only upgrades depend on it.
    if (version < 2) {
        AT_LOG_INFO("storage", "migrating schema to v2 (media)");

        static const char* const kV2[] = {
            // bytes_encrypted holds the same XChaCha20-Poly1305 ciphertext as a
            // message body. No mime column: that lives on the message row, and
            // duplicating it is a chance for the two to disagree.
            "CREATE TABLE IF NOT EXISTS media ("
            "  id              TEXT PRIMARY KEY,"
            "  byte_count      INTEGER NOT NULL DEFAULT 0,"
            "  created_at      INTEGER NOT NULL DEFAULT 0,"
            "  bytes_encrypted BLOB    NOT NULL)",

            // ALTER is not idempotent, which is exactly why it sits behind the
            // version check rather than an IF NOT EXISTS that SQLite lacks here.
            "ALTER TABLE message ADD COLUMN media_id TEXT",
            "ALTER TABLE message ADD COLUMN media_mime TEXT",
            "ALTER TABLE message ADD COLUMN media_bytes INTEGER NOT NULL DEFAULT 0",
        };

        for (const char* statement : kV2) {
            if (!exec(statement)) {
                return Result::error(ErrorCode::StorageFailure, "Could not migrate to schema v2.");
            }
        }
    }

    // ── v3: the outbox remembers which transport class it wants ──────────────
    //
    // Without this column the class is lost on restart and everything comes back
    // as Text — so a photo queued before the app was killed would quietly route
    // over the cheap radio instead of the bulk one. The default matches
    // PayloadClass::Text (1), which is the right answer for every row that
    // existed before this migration, because all of them were text.
    if (version < 3) {
        AT_LOG_INFO("storage", "migrating schema to v3 (outbox payload class)");

        if (!exec("ALTER TABLE outbox ADD COLUMN payload_class INTEGER NOT NULL DEFAULT 1")) {
            return Result::error(ErrorCode::StorageFailure, "Could not migrate to schema v3.");
        }
    }

    QSqlQuery q(db());
    if (!q.exec(QStringLiteral("PRAGMA user_version = %1").arg(kSchemaVersion))) {
        return failed(q, "Could not stamp the schema version.");
    }
    return Result::ok();
}

// ─────────────────────────────────────────────────────────────────────────────

SqliteStore::SqliteStore(QString databasePath, IDataCipher& cipher, IClock& clock)
    : impl_(std::make_unique<Impl>(std::move(databasePath), cipher, clock))
{
}

SqliteStore::~SqliteStore()
{
    close();
}

Result SqliteStore::open()
{
    if (impl_->opened) return Result::ok();

    QDir().mkpath(QFileInfo(impl_->path).absolutePath());

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), impl_->connection);
    db.setDatabaseName(impl_->path);

    if (!db.open()) {
        // Almost always the driver plugin missing rather than the file: the
        // QSQLITE plugin has to be deployed with the app on Android.
        AT_LOG_CRITICAL("storage", "cannot open the database: {}",
                        db.lastError().text().toStdString());
        // The handle must go out of use before removeDatabase, or Qt warns that
        // the connection is still referenced.
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(impl_->connection);
        return Result::error(ErrorCode::StorageFailure, "Could not open the database.");
    }

    impl_->opened = true;

    // WAL (section 12): readers never block the writer, and a crash cannot leave
    // a half-written transaction behind. NORMAL sync is the right trade with WAL
    // — a power cut can lose the last commit, not corrupt the file.
    impl_->exec("PRAGMA journal_mode = WAL");
    impl_->exec("PRAGMA synchronous = NORMAL");
    impl_->exec("PRAGMA foreign_keys = ON");

    if (const Result r = impl_->migrate(); r.isError()) {
        close();
        return r;
    }

    AT_LOG_INFO("storage", "database open at schema v{}", impl_->userVersion());
    return Result::ok();
}

void SqliteStore::close()
{
    if (!impl_->opened) return;

    // The QSqlDatabase handle must be out of scope before removeDatabase, or Qt
    // warns that the connection is still in use.
    {
        QSqlDatabase db = impl_->db();
        if (db.isOpen()) db.close();
    }
    QSqlDatabase::removeDatabase(impl_->connection);
    impl_->opened = false;
}

int SqliteStore::schemaVersion() const
{
    return impl_->opened ? impl_->userVersion() : 0;
}

Result SqliteStore::wipeAll()
{
    AT_LOG_WARN("storage", "wiping every table");

    static const char* const kTables[] = {"outbox",  "media",   "message", "chat",
                                          "request", "blocked", "contact", "peer",
                                          "account"};
    for (const char* table : kTables) {
        QSqlQuery q(impl_->db());
        if (!q.exec(QStringLiteral("DELETE FROM %1").arg(QLatin1String(table)))) {
            return failed(q, "Could not wipe a table.");
        }
    }
    impl_->exec("VACUUM");
    return Result::ok();
}

// ── IAccountRepository ───────────────────────────────────────────────────────

Outcome<std::optional<Account>> SqliteStore::load()
{
    QSqlQuery q(impl_->db());
    if (!q.exec(QStringLiteral(
            "SELECT peer_id, username, dob, gender, discoverable, accept_from,"
            "       static_pk, signing_pk, created_at FROM account WHERE id = 1"))) {
        return failed(q, "Could not read the account.");
    }
    if (!q.next()) return std::optional<Account>{};

    const auto peerId = PeerId::fromHex(q.value(0).toString().toStdString());
    if (!peerId) {
        return Outcome<std::optional<Account>>::error(ErrorCode::StorageFailure,
                                                      "Stored peer id is malformed.");
    }

    Account account;
    account.peerId       = *peerId;
    account.username     = q.value(1).toString().toStdString();
    account.dateOfBirth  = q.value(2).toString().toStdString();
    account.gender       = static_cast<Gender>(q.value(3).toInt());
    account.discoverable = q.value(4).toBool();
    account.acceptFrom   = static_cast<AcceptFrom>(q.value(5).toInt());
    account.createdAt    = q.value(8).toLongLong();

    keyFromBlob(q.value(6).toByteArray(), account.staticPublicKey);
    keyFromBlob(q.value(7).toByteArray(), account.signingPublicKey);

    return std::optional<Account>{account};
}

Result SqliteStore::save(const Account& account)
{
    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral(
        "INSERT INTO account (id, peer_id, username, dob, gender, discoverable,"
        "                     accept_from, static_pk, signing_pk, created_at)"
        " VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(id) DO UPDATE SET"
        "   peer_id = excluded.peer_id, username = excluded.username,"
        "   dob = excluded.dob, gender = excluded.gender,"
        "   discoverable = excluded.discoverable, accept_from = excluded.accept_from,"
        "   static_pk = excluded.static_pk, signing_pk = excluded.signing_pk,"
        "   created_at = excluded.created_at"));

    q.addBindValue(hexOf(account.peerId));
    q.addBindValue(QString::fromStdString(account.username));
    q.addBindValue(QString::fromStdString(account.dateOfBirth));
    q.addBindValue(static_cast<int>(account.gender));
    q.addBindValue(account.discoverable);
    q.addBindValue(static_cast<int>(account.acceptFrom));
    q.addBindValue(keyBlob(account.staticPublicKey));
    q.addBindValue(keyBlob(account.signingPublicKey));
    q.addBindValue(static_cast<qlonglong>(account.createdAt));

    if (!q.exec()) return failed(q, "Could not save the account.");
    return Result::ok();
}

Result SqliteStore::clear()
{
    QSqlQuery q(impl_->db());
    if (!q.exec(QStringLiteral("DELETE FROM account"))) {
        return failed(q, "Could not clear the account.");
    }
    return Result::ok();
}

// ── IContactRepository · peers ───────────────────────────────────────────────

Result SqliteStore::upsertPeer(const Peer& peer)
{
    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral(
        "INSERT INTO peer (peer_id, static_pk, nickname, signal,"
        "                  verified_announce_at, last_seen_at)"
        " VALUES (?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(peer_id) DO UPDATE SET"
        "   static_pk = excluded.static_pk, nickname = excluded.nickname,"
        "   signal = excluded.signal,"
        "   verified_announce_at = excluded.verified_announce_at,"
        "   last_seen_at = excluded.last_seen_at"));

    q.addBindValue(hexOf(peer.peerId));
    q.addBindValue(keyBlob(peer.staticPublicKey));
    q.addBindValue(QString::fromStdString(peer.nickname));
    q.addBindValue(static_cast<int>(peer.signal));
    q.addBindValue(static_cast<qlonglong>(peer.verifiedAnnounceAt));
    q.addBindValue(static_cast<qlonglong>(peer.lastSeenAt));

    if (!q.exec()) return failed(q, "Could not store the peer.");
    return Result::ok();
}

namespace {

Peer peerFromRow(const QSqlQuery& q, const PeerId& peerId)
{
    Peer peer;
    peer.peerId               = peerId;
    peer.nickname             = q.value(2).toString().toStdString();
    peer.signal               = static_cast<SignalBand>(q.value(3).toInt());
    peer.verifiedAnnounceAt   = q.value(4).toLongLong();
    peer.lastSeenAt           = q.value(5).toLongLong();
    keyFromBlob(q.value(1).toByteArray(), peer.staticPublicKey);
    return peer;
}

constexpr const char* kPeerColumns =
    "SELECT peer_id, static_pk, nickname, signal, verified_announce_at, last_seen_at FROM peer";

}  // namespace

Outcome<std::optional<Peer>> SqliteStore::findPeer(const PeerId& peerId)
{
    QSqlQuery q(impl_->db());
    q.prepare(QString::fromLatin1(kPeerColumns) + QStringLiteral(" WHERE peer_id = ?"));
    q.addBindValue(hexOf(peerId));

    if (!q.exec()) return failed(q, "Could not read the peer.");
    if (!q.next()) return std::optional<Peer>{};
    return std::optional<Peer>{peerFromRow(q, peerId)};
}

Outcome<std::vector<Peer>> SqliteStore::listPeersSeenSince(Timestamp since)
{
    QSqlQuery q(impl_->db());
    q.prepare(QString::fromLatin1(kPeerColumns)
              + QStringLiteral(" WHERE last_seen_at >= ? ORDER BY last_seen_at DESC"));
    q.addBindValue(static_cast<qlonglong>(since));

    if (!q.exec()) return failed(q, "Could not list peers.");

    std::vector<Peer> out;
    while (q.next()) {
        const auto peerId = PeerId::fromHex(q.value(0).toString().toStdString());
        if (peerId) out.push_back(peerFromRow(q, *peerId));
    }
    return out;
}

Result SqliteStore::prunePeersOlderThan(Timestamp cutoff)
{
    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral("DELETE FROM peer WHERE last_seen_at < ?"));
    q.addBindValue(static_cast<qlonglong>(cutoff));

    if (!q.exec()) return failed(q, "Could not prune peers.");
    return Result::ok();
}

// ── IContactRepository · contacts and blocks ─────────────────────────────────

Result SqliteStore::addContact(const Contact& contact)
{
    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral(
        "INSERT INTO contact (peer_id, pinned_static_pk, display_name, created_at)"
        " VALUES (?, ?, ?, ?)"
        // The pinned key is deliberately NOT updated on conflict. It is the
        // trust anchor: re-accepting must not silently re-pin a different key
        // (section 08).
        " ON CONFLICT(peer_id) DO UPDATE SET display_name = excluded.display_name"));

    q.addBindValue(hexOf(contact.peerId));
    q.addBindValue(keyBlob(contact.pinnedStaticKey));
    q.addBindValue(QString::fromStdString(contact.displayName));
    q.addBindValue(static_cast<qlonglong>(contact.createdAt));

    if (!q.exec()) return failed(q, "Could not store the contact.");

    if (contact.blockedAt.has_value()) return setBlocked(contact.peerId, true);
    return Result::ok();
}

Outcome<std::optional<Contact>> SqliteStore::findContact(const PeerId& peerId)
{
    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral(
        "SELECT c.pinned_static_pk, c.display_name, c.created_at, b.blocked_at"
        "  FROM contact c LEFT JOIN blocked b ON b.peer_id = c.peer_id"
        " WHERE c.peer_id = ?"));
    q.addBindValue(hexOf(peerId));

    if (!q.exec()) return failed(q, "Could not read the contact.");
    if (!q.next()) return std::optional<Contact>{};

    Contact contact;
    contact.peerId      = peerId;
    contact.displayName = q.value(1).toString().toStdString();
    contact.createdAt   = q.value(2).toLongLong();
    keyFromBlob(q.value(0).toByteArray(), contact.pinnedStaticKey);
    if (!q.value(3).isNull()) contact.blockedAt = q.value(3).toLongLong();

    return std::optional<Contact>{contact};
}

Outcome<std::vector<Contact>> SqliteStore::listContacts()
{
    QSqlQuery q(impl_->db());
    if (!q.exec(QStringLiteral(
            "SELECT c.peer_id, c.pinned_static_pk, c.display_name, c.created_at, b.blocked_at"
            "  FROM contact c LEFT JOIN blocked b ON b.peer_id = c.peer_id"
            " ORDER BY c.created_at DESC"))) {
        return failed(q, "Could not list contacts.");
    }

    std::vector<Contact> out;
    while (q.next()) {
        const auto peerId = PeerId::fromHex(q.value(0).toString().toStdString());
        if (!peerId) continue;

        Contact contact;
        contact.peerId      = *peerId;
        contact.displayName = q.value(2).toString().toStdString();
        contact.createdAt   = q.value(3).toLongLong();
        keyFromBlob(q.value(1).toByteArray(), contact.pinnedStaticKey);
        if (!q.value(4).isNull()) contact.blockedAt = q.value(4).toLongLong();

        out.push_back(contact);
    }
    return out;
}

Result SqliteStore::setBlocked(const PeerId& peerId, bool blocked)
{
    QSqlQuery q(impl_->db());
    if (blocked) {
        q.prepare(QStringLiteral("INSERT INTO blocked (peer_id, blocked_at) VALUES (?, ?)"
                                 " ON CONFLICT(peer_id) DO NOTHING"));
        q.addBindValue(hexOf(peerId));
        q.addBindValue(static_cast<qlonglong>(impl_->clock.now()));
    } else {
        q.prepare(QStringLiteral("DELETE FROM blocked WHERE peer_id = ?"));
        q.addBindValue(hexOf(peerId));
    }

    if (!q.exec()) return failed(q, "Could not update the block list.");
    return Result::ok();
}

Result SqliteStore::runInTransaction(const std::function<Result()>& work)
{
    if (impl_->transactionDepth > 0) {
        // Join the open transaction. A failure here cannot be undone on its own
        // — SQLite has no partial rollback without savepoints, and none of our
        // units need one — so it dooms the outer unit instead.
        ++impl_->transactionDepth;
        const Result r = work();
        --impl_->transactionDepth;
        if (r.isError()) impl_->transactionFailed = true;
        return r;
    }

    QSqlDatabase db = impl_->db();
    if (!db.transaction()) {
        return Result::error(ErrorCode::StorageFailure,
                             "Could not begin a transaction: " + db.lastError().text().toStdString());
    }

    impl_->transactionDepth = 1;
    impl_->transactionFailed = false;
    Result r = work();
    impl_->transactionDepth = 0;

    if (r.isOk() && impl_->transactionFailed) {
        r = Result::error(ErrorCode::StorageFailure, "A step inside the transaction failed.");
    }
    impl_->transactionFailed = false;

    if (r.isError()) {
        if (!db.rollback()) {
            AT_LOG_ERROR("storage", "rollback failed: {}", db.lastError().text().toStdString());
        }
        return r;
    }

    if (!db.commit()) {
        const std::string why = db.lastError().text().toStdString();
        db.rollback();
        AT_LOG_ERROR("storage", "commit failed: {}", why);
        return Result::error(ErrorCode::StorageFailure, "Could not commit: " + why);
    }
    return Result::ok();
}

bool SqliteStore::isBlocked(const PeerId& peerId)
{
    return impl_->isBlockedInternal(peerId);
}

Outcome<std::vector<BlockedPeer>> SqliteStore::listBlocked()
{
    // The name is picked in TRUST order, not recency. The pinned contact name
    // was fixed at accept time against an authenticated key; a request nickname
    // or an announce is whatever the peer chose to call themselves last — which,
    // for someone worth blocking, may be chosen to look like someone else.
    QSqlQuery q(impl_->db());
    if (!q.exec(QStringLiteral(
            "SELECT b.peer_id, b.blocked_at,"
            "       COALESCE(NULLIF(c.display_name, ''),"
            "                (SELECT NULLIF(r.nickname, '') FROM request r"
            "                  WHERE r.peer_id = b.peer_id AND r.direction = 0"
            "                  ORDER BY r.created_at DESC LIMIT 1),"
            "                NULLIF(p.nickname, ''), '')"
            "  FROM blocked b"
            "  LEFT JOIN contact c ON c.peer_id = b.peer_id"
            "  LEFT JOIN peer    p ON p.peer_id = b.peer_id"
            " ORDER BY b.blocked_at DESC, b.peer_id"))) {
        return failed(q, "Could not list blocked peers.");
    }

    std::vector<BlockedPeer> out;
    while (q.next()) {
        const auto peerId = PeerId::fromHex(q.value(0).toString().toStdString());
        if (!peerId) continue;

        BlockedPeer row;
        row.peerId      = *peerId;
        row.blockedAt   = q.value(1).toLongLong();
        row.displayName = q.value(2).toString().toStdString();
        out.push_back(row);
    }
    return out;
}

// ── IContactRepository · requests ────────────────────────────────────────────

Result SqliteStore::saveRequest(const Request& request)
{
    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral(
        "INSERT INTO request (id, peer_id, direction, nickname, greeting, state, created_at)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(id) DO UPDATE SET"
        "   nickname = excluded.nickname, greeting = excluded.greeting,"
        "   state = excluded.state"));

    q.addBindValue(QString::fromStdString(request.id.value()));
    q.addBindValue(hexOf(request.peerId));
    q.addBindValue(static_cast<int>(request.direction));
    q.addBindValue(QString::fromStdString(request.nickname));
    q.addBindValue(QString::fromStdString(request.greeting));
    q.addBindValue(static_cast<int>(request.state));
    q.addBindValue(static_cast<qlonglong>(request.createdAt));

    if (!q.exec()) return failed(q, "Could not store the request.");
    return Result::ok();
}

namespace {

Request requestFromRow(const QSqlQuery& q, const PeerId& peerId)
{
    Request request;
    request.id        = RequestId{q.value(0).toString().toStdString()};
    request.peerId    = peerId;
    request.direction = static_cast<Direction>(q.value(2).toInt());
    request.nickname  = q.value(3).toString().toStdString();
    request.greeting  = q.value(4).toString().toStdString();
    request.state     = static_cast<RequestState>(q.value(5).toInt());
    request.createdAt = q.value(6).toLongLong();
    return request;
}

constexpr const char* kRequestColumns =
    "SELECT id, peer_id, direction, nickname, greeting, state, created_at FROM request";

}  // namespace

Outcome<std::optional<Request>> SqliteStore::findRequest(const RequestId& id)
{
    QSqlQuery q(impl_->db());
    q.prepare(QString::fromLatin1(kRequestColumns) + QStringLiteral(" WHERE id = ?"));
    q.addBindValue(QString::fromStdString(id.value()));

    if (!q.exec()) return failed(q, "Could not read the request.");
    if (!q.next()) return std::optional<Request>{};

    const auto peerId = PeerId::fromHex(q.value(1).toString().toStdString());
    if (!peerId) return std::optional<Request>{};
    return std::optional<Request>{requestFromRow(q, *peerId)};
}

Outcome<std::vector<Request>> SqliteStore::listRequests(Direction direction, RequestState state)
{
    QSqlQuery q(impl_->db());
    q.prepare(QString::fromLatin1(kRequestColumns)
              + QStringLiteral(" WHERE direction = ? AND state = ? ORDER BY created_at DESC"));
    q.addBindValue(static_cast<int>(direction));
    q.addBindValue(static_cast<int>(state));

    if (!q.exec()) return failed(q, "Could not list requests.");

    std::vector<Request> out;
    while (q.next()) {
        const auto peerId = PeerId::fromHex(q.value(1).toString().toStdString());
        if (peerId) out.push_back(requestFromRow(q, *peerId));
    }
    return out;
}

Result SqliteStore::setRequestState(const RequestId& id, RequestState state)
{
    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral("UPDATE request SET state = ? WHERE id = ?"));
    q.addBindValue(static_cast<int>(state));
    q.addBindValue(QString::fromStdString(id.value()));

    if (!q.exec()) return failed(q, "Could not update the request.");
    if (q.numRowsAffected() == 0) return Result::error(ErrorCode::NotFound, "No such request.");
    return Result::ok();
}

Outcome<int> SqliteStore::countInboundRequestsSince(const PeerId& peerId, Timestamp since)
{
    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM request"
                             " WHERE peer_id = ? AND direction = ? AND created_at >= ?"));
    q.addBindValue(hexOf(peerId));
    q.addBindValue(static_cast<int>(Direction::Incoming));
    q.addBindValue(static_cast<qlonglong>(since));

    if (!q.exec() || !q.next()) return failed(q, "Could not count requests.");
    return q.value(0).toInt();
}

// ── IMessageRepository · chats ───────────────────────────────────────────────

namespace {

Chat chatFromRow(const QSqlQuery& q, const PeerId& peerId)
{
    Chat chat;
    chat.id            = ChatId{q.value(0).toString().toStdString()};
    chat.peerId        = peerId;
    chat.createdAt     = q.value(2).toLongLong();
    chat.lastMessageAt = q.value(3).toLongLong();
    chat.unreadCount   = q.value(4).toInt();
    return chat;
}

constexpr const char* kChatColumns =
    "SELECT id, peer_id, created_at, last_message_at, unread FROM chat";

}  // namespace

Outcome<Chat> SqliteStore::createChat(const PeerId& peerId)
{
    // One chat per peer, always. Two chats with the same person would split the
    // history in a way nothing downstream expects.
    auto existing = findChatByPeer(peerId);
    if (existing.isError()) return existing.result();
    if (existing.value().has_value()) return *existing.value();

    Chat chat;
    chat.id            = ChatId{"chat_" + peerId.toHex()};
    chat.peerId        = peerId;
    chat.createdAt     = impl_->clock.now();
    chat.lastMessageAt = chat.createdAt;
    chat.unreadCount   = 0;

    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral(
        "INSERT INTO chat (id, peer_id, created_at, last_message_at, unread)"
        " VALUES (?, ?, ?, ?, 0)"));
    q.addBindValue(QString::fromStdString(chat.id.value()));
    q.addBindValue(hexOf(peerId));
    q.addBindValue(static_cast<qlonglong>(chat.createdAt));
    q.addBindValue(static_cast<qlonglong>(chat.lastMessageAt));

    if (!q.exec()) return failed(q, "Could not create the chat.");

    AT_LOG_DEBUG("storage", "created chat {} for peer {}", chat.id.value(), peerId.toHex());
    return chat;
}

Outcome<std::optional<Chat>> SqliteStore::findChat(const ChatId& id)
{
    QSqlQuery q(impl_->db());
    q.prepare(QString::fromLatin1(kChatColumns) + QStringLiteral(" WHERE id = ?"));
    q.addBindValue(QString::fromStdString(id.value()));

    if (!q.exec()) return failed(q, "Could not read the chat.");
    if (!q.next()) return std::optional<Chat>{};

    const auto peerId = PeerId::fromHex(q.value(1).toString().toStdString());
    if (!peerId) return std::optional<Chat>{};
    return std::optional<Chat>{chatFromRow(q, *peerId)};
}

Outcome<std::optional<Chat>> SqliteStore::findChatByPeer(const PeerId& peerId)
{
    QSqlQuery q(impl_->db());
    q.prepare(QString::fromLatin1(kChatColumns) + QStringLiteral(" WHERE peer_id = ?"));
    q.addBindValue(hexOf(peerId));

    if (!q.exec()) return failed(q, "Could not read the chat.");
    if (!q.next()) return std::optional<Chat>{};
    return std::optional<Chat>{chatFromRow(q, peerId)};
}

Outcome<std::vector<Chat>> SqliteStore::listChats()
{
    QSqlQuery q(impl_->db());
    if (!q.exec(QString::fromLatin1(kChatColumns)
                + QStringLiteral(" ORDER BY last_message_at DESC"))) {
        return failed(q, "Could not list chats.");
    }

    std::vector<Chat> out;
    while (q.next()) {
        const auto peerId = PeerId::fromHex(q.value(1).toString().toStdString());
        if (peerId) out.push_back(chatFromRow(q, *peerId));
    }
    return out;
}

Result SqliteStore::setUnreadCount(const ChatId& id, int count)
{
    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral("UPDATE chat SET unread = ? WHERE id = ?"));
    q.addBindValue(count);
    q.addBindValue(QString::fromStdString(id.value()));

    if (!q.exec()) return failed(q, "Could not update the unread count.");
    if (q.numRowsAffected() == 0) return Result::error(ErrorCode::NotFound, "No such chat.");
    return Result::ok();
}

// ── IMessageRepository · messages ────────────────────────────────────────────

Outcome<MessageId> SqliteStore::append(const Message& message)
{
    auto seq = impl_->nextSeq("message");
    if (seq.isError()) return seq.result();

    QByteArray sealed;
    if (!impl_->seal(message.body, sealed)) {
        return Outcome<MessageId>::error(ErrorCode::StorageFailure,
                                         "Could not encrypt the message body.");
    }

    const MessageId id{"msg_" + std::to_string(seq.value())};

    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral(
        "INSERT INTO message (seq, id, chat_id, client_msg_id, direction, body_encrypted,"
        "                     status, created_at, delivered_at, read_at,"
        "                     media_id, media_mime, media_bytes)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(static_cast<qlonglong>(seq.value()));
    q.addBindValue(QString::fromStdString(id.value()));
    q.addBindValue(QString::fromStdString(message.chatId.value()));
    q.addBindValue(QString::fromStdString(message.clientMessageId.value()));
    q.addBindValue(static_cast<int>(message.direction));
    q.addBindValue(sealed);
    q.addBindValue(static_cast<int>(message.status));
    q.addBindValue(static_cast<qlonglong>(message.createdAt));
    q.addBindValue(static_cast<qlonglong>(message.deliveredAt));
    q.addBindValue(static_cast<qlonglong>(message.readAt));
    q.addBindValue(QString::fromStdString(message.mediaId));
    q.addBindValue(QString::fromStdString(message.mediaMimeType));
    q.addBindValue(static_cast<qlonglong>(message.mediaBytes));

    if (!q.exec()) return failed(q, "Could not store the message.");

    // Keeps the chat list ordered without the caller remembering to do it.
    QSqlQuery touch(impl_->db());
    touch.prepare(QStringLiteral(
        "UPDATE chat SET last_message_at = ? WHERE id = ? AND last_message_at < ?"));
    touch.addBindValue(static_cast<qlonglong>(message.createdAt));
    touch.addBindValue(QString::fromStdString(message.chatId.value()));
    touch.addBindValue(static_cast<qlonglong>(message.createdAt));
    touch.exec();

    AT_LOG_DEBUG("storage", "stored message {} ({} bytes) in chat {}", id.value(),
                 message.body.size(), message.chatId.value());
    return id;
}

Result SqliteStore::setStatus(const MessageId& id, DeliveryStatus status)
{
    const Timestamp now = impl_->clock.now();

    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral(
        "UPDATE message SET status = ?,"
        "  delivered_at = CASE WHEN ? = 1 AND delivered_at = 0 THEN ? ELSE delivered_at END,"
        "  read_at      = CASE WHEN ? = 1 AND read_at = 0      THEN ? ELSE read_at END"
        " WHERE id = ?"));
    q.addBindValue(static_cast<int>(status));
    q.addBindValue(status == DeliveryStatus::Delivered ? 1 : 0);
    q.addBindValue(static_cast<qlonglong>(now));
    q.addBindValue(status == DeliveryStatus::Read ? 1 : 0);
    q.addBindValue(static_cast<qlonglong>(now));
    q.addBindValue(QString::fromStdString(id.value()));

    if (!q.exec()) return failed(q, "Could not update the message status.");
    if (q.numRowsAffected() == 0) return Result::error(ErrorCode::NotFound, "No such message.");
    return Result::ok();
}

namespace {

constexpr const char* kMessageColumns =
    "SELECT seq, id, chat_id, client_msg_id, direction, body_encrypted, status,"
    "       created_at, delivered_at, read_at, media_id, media_mime, media_bytes"
    "  FROM message";

}  // namespace

Message SqliteStore::readMessage(QSqlQuery& q)
{
    Message message;
    message.id              = MessageId{q.value(1).toString().toStdString()};
    message.chatId          = ChatId{q.value(2).toString().toStdString()};
    message.clientMessageId = ClientMessageId{q.value(3).toString().toStdString()};
    message.direction       = static_cast<Direction>(q.value(4).toInt());
    message.body            = impl_->openText(q.value(5).toByteArray());
    message.status          = static_cast<DeliveryStatus>(q.value(6).toInt());
    message.createdAt       = q.value(7).toLongLong();
    message.deliveredAt     = q.value(8).toLongLong();
    message.readAt          = q.value(9).toLongLong();
    message.mediaId         = q.value(10).toString().toStdString();
    message.mediaMimeType   = q.value(11).toString().toStdString();
    message.mediaBytes      = static_cast<std::size_t>(q.value(12).toLongLong());
    return message;
}

Result SqliteStore::putMedia(const std::string& mediaId, ByteSpan bytes)
{
    if (mediaId.empty()) return Result::error(ErrorCode::InvalidArgument, "Media id is empty.");
    if (bytes.empty()) return Result::error(ErrorCode::InvalidArgument, "Image is empty.");
    if (bytes.size() > protocol::kMaxMediaBytes) {
        return Result::error(ErrorCode::InvalidArgument, "Image exceeds the size limit.");
    }

    ByteBuffer sealed;
    if (impl_->cipher.seal(bytes, sealed).isError()) {
        return Result::error(ErrorCode::StorageFailure, "Could not encrypt the image.");
    }

    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral(
        "INSERT INTO media (id, byte_count, created_at, bytes_encrypted)"
        " VALUES (?, ?, ?, ?)"
        // Re-receiving the same image (a resend after a lost ack) replaces it
        // rather than failing the whole delivery on a primary-key clash.
        " ON CONFLICT(id) DO UPDATE SET"
        "   byte_count = excluded.byte_count,"
        "   bytes_encrypted = excluded.bytes_encrypted"));

    q.addBindValue(QString::fromStdString(mediaId));
    q.addBindValue(static_cast<qlonglong>(bytes.size()));
    q.addBindValue(static_cast<qlonglong>(impl_->clock.now()));
    q.addBindValue(fromBuffer(sealed));

    if (!q.exec()) return failed(q, "Could not store the image.");

    AT_LOG_DEBUG("storage", "stored image {} ({} bytes)", mediaId, bytes.size());
    return Result::ok();
}

Outcome<ByteBuffer> SqliteStore::getMedia(const std::string& mediaId)
{
    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral("SELECT bytes_encrypted FROM media WHERE id = ?"));
    q.addBindValue(QString::fromStdString(mediaId));

    if (!q.exec()) return failed(q, "Could not read the image.");
    if (!q.next()) return Outcome<ByteBuffer>::error(ErrorCode::NotFound, "No such image.");

    const QByteArray blob = q.value(0).toByteArray();
    ByteBuffer plain;
    const auto* p = reinterpret_cast<const Byte*>(blob.constData());

    // Unlike a message body, a failed decrypt here is reported rather than
    // swallowed: there is no partial image worth showing, and the caller needs
    // to answer the HTTP request with an error rather than a blank picture.
    if (impl_->cipher.open(ByteSpan{p, static_cast<std::size_t>(blob.size())}, plain).isError()) {
        return Outcome<ByteBuffer>::error(ErrorCode::DecryptFailed,
                                          "That image could not be decrypted.");
    }
    return plain;
}

Outcome<MessagePage> SqliteStore::page(const ChatId& chatId, const Cursor& cursor, int limit)
{
    if (limit <= 0) limit = 50;

    // The cursor is the id of the last message the caller already holds. Paging
    // on `seq` rather than `created_at` keeps this stable when two messages
    // share a millisecond.
    qint64 beforeSeq = std::numeric_limits<qint64>::max();
    if (!cursor.empty()) {
        QSqlQuery find(impl_->db());
        find.prepare(QStringLiteral("SELECT seq FROM message WHERE id = ?"));
        find.addBindValue(QString::fromStdString(cursor));
        if (find.exec() && find.next()) beforeSeq = find.value(0).toLongLong();
    }

    QSqlQuery q(impl_->db());
    q.prepare(QString::fromLatin1(kMessageColumns)
              + QStringLiteral(" WHERE chat_id = ? AND seq < ? ORDER BY seq DESC LIMIT ?"));
    q.addBindValue(QString::fromStdString(chatId.value()));
    q.addBindValue(beforeSeq);
    q.addBindValue(limit + 1);  // one extra row answers hasMore without a COUNT

    if (!q.exec()) return failed(q, "Could not page the chat history.");

    MessagePage result;
    while (q.next()) {
        if (static_cast<int>(result.messages.size()) == limit) {
            result.hasMore = true;
            break;
        }
        result.messages.push_back(readMessage(q));
    }
    if (result.hasMore && !result.messages.empty()) {
        result.nextCursor = result.messages.back().id.value();
    }
    return result;
}

Outcome<std::optional<Message>> SqliteStore::findMessage(const MessageId& id)
{
    QSqlQuery q(impl_->db());
    q.prepare(QString::fromLatin1(kMessageColumns) + QStringLiteral(" WHERE id = ?"));
    q.addBindValue(QString::fromStdString(id.value()));

    if (!q.exec()) return failed(q, "Could not read the message.");
    if (!q.next()) return std::optional<Message>{};
    return std::optional<Message>{readMessage(q)};
}

Outcome<std::optional<Message>> SqliteStore::findByClientId(const ClientMessageId& clientId)
{
    if (clientId.value().empty()) return std::optional<Message>{};

    QSqlQuery q(impl_->db());
    q.prepare(QString::fromLatin1(kMessageColumns)
              + QStringLiteral(" WHERE client_msg_id = ? ORDER BY seq DESC LIMIT 1"));
    q.addBindValue(QString::fromStdString(clientId.value()));

    if (!q.exec()) return failed(q, "Could not read the message.");
    if (!q.next()) return std::optional<Message>{};
    return std::optional<Message>{readMessage(q)};
}

// ── IOutbox ──────────────────────────────────────────────────────────────────

Result SqliteStore::enqueue(const OutboxEntry& entry)
{
    // The encoded envelope is encrypted at rest exactly like a message body: it
    // contains the same plaintext until it reaches the Noise session.
    ByteBuffer sealed;
    if (impl_->cipher.seal(ByteSpan{entry.payload}, sealed).isError()) {
        return Result::error(ErrorCode::StorageFailure, "Could not encrypt the outbox payload.");
    }

    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral(
        "INSERT INTO outbox (message_id, peer_id, attempts, next_attempt_at, expires_at,"
        "                    payload_encrypted, payload_class)"
        " VALUES (?, ?, ?, ?, ?, ?, ?)"
        " ON CONFLICT(message_id) DO UPDATE SET"
        "   attempts = excluded.attempts, next_attempt_at = excluded.next_attempt_at,"
        "   expires_at = excluded.expires_at,"
        "   payload_encrypted = excluded.payload_encrypted,"
        "   payload_class = excluded.payload_class"));

    q.addBindValue(QString::fromStdString(entry.messageId.value()));
    q.addBindValue(hexOf(entry.peerId));
    q.addBindValue(entry.attempts);
    q.addBindValue(static_cast<qlonglong>(entry.nextAttemptAt));
    q.addBindValue(static_cast<qlonglong>(entry.expiresAt));
    q.addBindValue(fromBuffer(sealed));
    q.addBindValue(static_cast<int>(entry.payloadClass));

    if (!q.exec()) return failed(q, "Could not queue the message.");
    return Result::ok();
}

Result SqliteStore::remove(const MessageId& id)
{
    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral("DELETE FROM outbox WHERE message_id = ?"));
    q.addBindValue(QString::fromStdString(id.value()));

    if (!q.exec()) return failed(q, "Could not remove the outbox entry.");
    return Result::ok();
}

OutboxEntry SqliteStore::readOutbox(QSqlQuery& q, const PeerId& peerId)
{
    OutboxEntry entry;
    entry.messageId     = MessageId{q.value(0).toString().toStdString()};
    entry.peerId        = peerId;
    entry.attempts      = q.value(2).toInt();
    entry.nextAttemptAt = q.value(3).toLongLong();
    entry.expiresAt     = q.value(4).toLongLong();
    entry.payloadClass  = static_cast<PayloadClass>(q.value(6).toInt());

    ByteBuffer plain;
    const QByteArray blob = q.value(5).toByteArray();
    const auto* p = reinterpret_cast<const Byte*>(blob.constData());
    if (impl_->cipher.open(ByteSpan{p, static_cast<std::size_t>(blob.size())}, plain).isOk()) {
        entry.payload = std::move(plain);
    }
    return entry;
}

namespace {

constexpr const char* kOutboxColumns =
    "SELECT message_id, peer_id, attempts, next_attempt_at, expires_at, payload_encrypted,"
    "       payload_class"
    "  FROM outbox";

}  // namespace

Outcome<std::vector<OutboxEntry>> SqliteStore::dueFor(const PeerId& peerId, Timestamp now)
{
    QSqlQuery q(impl_->db());
    q.prepare(QString::fromLatin1(kOutboxColumns)
              + QStringLiteral(" WHERE peer_id = ? AND next_attempt_at <= ?"
                               " ORDER BY next_attempt_at ASC, message_id ASC"));
    q.addBindValue(hexOf(peerId));
    q.addBindValue(static_cast<qlonglong>(now));

    if (!q.exec()) return failed(q, "Could not read the outbox.");

    std::vector<OutboxEntry> out;
    while (q.next()) out.push_back(readOutbox(q, peerId));
    return out;
}

Outcome<std::vector<PeerId>> SqliteStore::peersWithPending()
{
    QSqlQuery q(impl_->db());
    if (!q.exec(QStringLiteral("SELECT DISTINCT peer_id FROM outbox"))) {
        return failed(q, "Could not read the outbox.");
    }

    std::vector<PeerId> out;
    while (q.next()) {
        const auto peerId = PeerId::fromHex(q.value(0).toString().toStdString());
        if (peerId) out.push_back(*peerId);
    }
    return out;
}

Result SqliteStore::recordAttempt(const MessageId& id, Timestamp nextAttemptAt)
{
    QSqlQuery q(impl_->db());
    q.prepare(QStringLiteral(
        "UPDATE outbox SET attempts = attempts + 1, next_attempt_at = ? WHERE message_id = ?"));
    q.addBindValue(static_cast<qlonglong>(nextAttemptAt));
    q.addBindValue(QString::fromStdString(id.value()));

    if (!q.exec()) return failed(q, "Could not record the send attempt.");
    if (q.numRowsAffected() == 0) {
        return Result::error(ErrorCode::NotFound, "No such outbox entry.");
    }
    return Result::ok();
}

Outcome<std::vector<OutboxEntry>> SqliteStore::reapExpired(Timestamp now)
{
    // Read the doomed rows first: the caller flips their messages to Failed, and
    // it cannot do that from rows that have already been deleted.
    QSqlQuery q(impl_->db());
    q.prepare(QString::fromLatin1(kOutboxColumns)
              + QStringLiteral(" WHERE (expires_at > 0 AND expires_at <= ?) OR attempts >= ?"));
    q.addBindValue(static_cast<qlonglong>(now));
    q.addBindValue(kMaxSendAttempts);

    if (!q.exec()) return failed(q, "Could not scan the outbox for expiry.");

    std::vector<OutboxEntry> expired;
    while (q.next()) {
        const auto peerId = PeerId::fromHex(q.value(1).toString().toStdString());
        expired.push_back(readOutbox(q, peerId ? *peerId : PeerId{}));
    }

    for (const OutboxEntry& entry : expired) {
        QSqlQuery del(impl_->db());
        del.prepare(QStringLiteral("DELETE FROM outbox WHERE message_id = ?"));
        del.addBindValue(QString::fromStdString(entry.messageId.value()));
        del.exec();
    }

    if (!expired.empty()) {
        AT_LOG_INFO("storage", "reaped {} expired outbox entries", expired.size());
    }
    return expired;
}

}  // namespace anytime::persistence
