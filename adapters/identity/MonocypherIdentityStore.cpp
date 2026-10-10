// adapters/identity/MonocypherIdentityStore.cpp

#include "adapters/identity/MonocypherIdentityStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <cstring>

#include "core/log/Log.h"

// Logging here is about the store's health only. Nothing in this file may log
// key bytes — secret or public — or a shared secret.

extern "C" {
#include "third_party/monocypher/monocypher-ed25519.h"
#include "third_party/monocypher/monocypher.h"
}

namespace anytime::identity {

namespace {

constexpr int kX25519SecretBytes  = 32;
constexpr int kEd25519SecretBytes = 64;  // Monocypher keeps seed || public
constexpr int kKeyFileBytes =
    kX25519SecretBytes + kEd25519SecretBytes + kPublicKeyBytes * 2;  // 160

/// OS CSPRNG. QRandomGenerator::system() is the seeded-from-the-OS generator —
/// never QRandomGenerator::global(), which is fine for jitter and useless here.
void randomBytes(std::uint8_t* out, int count)
{
    QByteArray buffer(count, Qt::Uninitialized);
    QRandomGenerator::system()->generate(
        reinterpret_cast<quint32*>(buffer.data()),
        reinterpret_cast<quint32*>(buffer.data() + buffer.size()));
    std::memcpy(out, buffer.constData(), count);
    crypto_wipe(reinterpret_cast<std::uint8_t*>(buffer.data()), count);
}

/// PeerId = first 8 bytes of SHA-256(X25519 public key). Monocypher offers
/// BLAKE2b and SHA-512 but not SHA-256, so Qt supplies it — the adapter layer
/// may use Qt freely.
PeerId derivePeerId(const PublicKey& staticPublicKey)
{
    const QByteArray fingerprint = QCryptographicHash::hash(
        QByteArray(reinterpret_cast<const char*>(staticPublicKey.data()),
                   static_cast<int>(kPublicKeyBytes)),
        QCryptographicHash::Sha256);

    return PeerId::fromFingerprint(
        ByteSpan{reinterpret_cast<const Byte*>(fingerprint.constData()),
                 static_cast<std::size_t>(fingerprint.size())});
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct MonocypherIdentityStore::Impl {
    QString path;
    bool    loaded = false;

    Identity      identity;
    std::uint8_t  x25519Secret[kX25519SecretBytes]{};
    std::uint8_t  ed25519Secret[kEd25519SecretBytes]{};

    ~Impl() { scrub(); }

    /// Zeroes secret material. crypto_wipe is not optimised away, unlike memset.
    void scrub()
    {
        crypto_wipe(x25519Secret, sizeof x25519Secret);
        crypto_wipe(ed25519Secret, sizeof ed25519Secret);
    }

    bool readFromDisk();
    bool writeToDisk() const;
    void generate();
};

bool MonocypherIdentityStore::Impl::readFromDisk()
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;

    QByteArray blob = file.readAll();
    if (blob.size() != kKeyFileBytes) {
        // Not silently replaced without a trace: regenerating means a new peer
        // id, and every contact would stop recognising this device.
        AT_LOG_ERROR("identity", "identity file is {} bytes, expected {}; it will be replaced",
                     blob.size(), kKeyFileBytes);
        crypto_wipe(reinterpret_cast<std::uint8_t*>(blob.data()), blob.size());
        return false;
    }

    const auto* p = reinterpret_cast<const std::uint8_t*>(blob.constData());
    std::memcpy(x25519Secret, p, kX25519SecretBytes);
    std::memcpy(ed25519Secret, p + kX25519SecretBytes, kEd25519SecretBytes);
    std::memcpy(identity.staticPublicKey.data(),
                p + kX25519SecretBytes + kEd25519SecretBytes, kPublicKeyBytes);
    std::memcpy(identity.signingPublicKey.data(),
                p + kX25519SecretBytes + kEd25519SecretBytes + kPublicKeyBytes, kPublicKeyBytes);

    crypto_wipe(reinterpret_cast<std::uint8_t*>(blob.data()), blob.size());

    identity.peerId = derivePeerId(identity.staticPublicKey);
    loaded = true;
    AT_LOG_DEBUG("identity", "loaded identity {}", identity.peerId.toHex());
    return true;
}

bool MonocypherIdentityStore::Impl::writeToDisk() const
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        AT_LOG_ERROR("identity", "cannot write identity file: {}",
                     file.errorString().toStdString());
        return false;
    }

    file.write(reinterpret_cast<const char*>(x25519Secret), kX25519SecretBytes);
    file.write(reinterpret_cast<const char*>(ed25519Secret), kEd25519SecretBytes);
    file.write(reinterpret_cast<const char*>(identity.staticPublicKey.data()),
               static_cast<qint64>(kPublicKeyBytes));
    file.write(reinterpret_cast<const char*>(identity.signingPublicKey.data()),
               static_cast<qint64>(kPublicKeyBytes));
    file.close();

    // Owner-only. Still plaintext on disk — Keystore wrapping is the outstanding
    // half of the at-rest story.
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

void MonocypherIdentityStore::Impl::generate()
{
    // ── X25519 static key ───────────────────────────────────────────────────
    randomBytes(x25519Secret, kX25519SecretBytes);
    crypto_x25519_public_key(identity.staticPublicKey.data(), x25519Secret);

    // ── Ed25519 signing key ─────────────────────────────────────────────────
    // crypto_ed25519_key_pair WIPES the seed it is given, so the seed buffer is
    // single-use by design.
    std::uint8_t seed[32];
    randomBytes(seed, sizeof seed);
    crypto_ed25519_key_pair(ed25519Secret, identity.signingPublicKey.data(), seed);

    identity.peerId = derivePeerId(identity.staticPublicKey);
    loaded = true;
    AT_LOG_INFO("identity", "generated a new identity {}", identity.peerId.toHex());
}

// ─────────────────────────────────────────────────────────────────────────────

MonocypherIdentityStore::MonocypherIdentityStore(QString storagePath)
    : impl_(std::make_unique<Impl>())
{
    impl_->path = std::move(storagePath);
}

MonocypherIdentityStore::~MonocypherIdentityStore() = default;

bool MonocypherIdentityStore::exists() const
{
    return impl_->loaded || QFile::exists(impl_->path);
}

Outcome<Identity> MonocypherIdentityStore::loadOrCreate()
{
    if (impl_->loaded) return impl_->identity;
    if (impl_->readFromDisk()) return impl_->identity;

    impl_->generate();
    if (!impl_->writeToDisk()) {
        return Outcome<Identity>::error(ErrorCode::StorageFailure,
                                        "Could not write the identity file.");
    }
    return impl_->identity;
}

Outcome<Signature> MonocypherIdentityStore::sign(ByteSpan message)
{
    if (!impl_->loaded && !impl_->readFromDisk()) {
        return Outcome<Signature>::error(ErrorCode::PreconditionFailed, "No identity yet.");
    }

    Signature signature{};
    crypto_ed25519_sign(signature.data(), impl_->ed25519Secret, message.data(), message.size());
    return signature;
}

bool MonocypherIdentityStore::verify(ByteSpan message, const Signature& signature,
                                     const PublicKey& signingKey)
{
    // Returns 0 on success. Constant-time inside Monocypher.
    return crypto_ed25519_check(signature.data(), signingKey.data(), message.data(),
                                message.size())
           == 0;
}

Outcome<std::array<Byte, 32>> MonocypherIdentityStore::agree(const PublicKey& theirStatic)
{
    if (!impl_->loaded && !impl_->readFromDisk()) {
        return Outcome<std::array<Byte, 32>>::error(ErrorCode::PreconditionFailed,
                                                    "No identity yet.");
    }

    std::array<Byte, 32> shared{};
    crypto_x25519(shared.data(), impl_->x25519Secret, theirStatic.data());

    // An all-zero shared secret means a low-order public key was supplied —
    // a contributory-behaviour attack, not a valid peer.
    bool allZero = true;
    for (const Byte b : shared) {
        if (b != 0) { allZero = false; break; }
    }
    if (allZero) {
        AT_LOG_WARN("identity", "rejected a low-order public key during key agreement");
        return Outcome<std::array<Byte, 32>>::error(ErrorCode::HandshakeFailed,
                                                    "Peer supplied a degenerate public key.");
    }
    return shared;
}

PeerId MonocypherIdentityStore::peerIdFor(const PublicKey& staticKey) const
{
    return derivePeerId(staticKey);
}

Result MonocypherIdentityStore::wipe()
{
    AT_LOG_WARN("identity", "wiping the identity");

    impl_->scrub();
    impl_->loaded = false;

    if (QFile::exists(impl_->path) && !QFile::remove(impl_->path)) {
        AT_LOG_ERROR("identity", "could not remove the identity file");
        return Result::error(ErrorCode::StorageFailure, "Could not remove the identity file.");
    }
    return Result::ok();
}

}  // namespace anytime::identity
