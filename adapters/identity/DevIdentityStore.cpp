// adapters/identity/DevIdentityStore.cpp
//
// Read the header first. This is not real cryptography.

#include "adapters/identity/DevIdentityStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <cstring>

namespace anytime::identity {

namespace {

QByteArray randomBytes(int count)
{
    QByteArray out(count, Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32*>(out.data()),
                                         reinterpret_cast<quint32*>(out.data() + out.size()));
    return out;
}

QByteArray toQt(ByteSpan span)
{
    return QByteArray(reinterpret_cast<const char*>(span.data()), static_cast<int>(span.size()));
}

}  // namespace

struct DevIdentityStore::Impl {
    QString    path;
    bool       loaded = false;
    Identity   identity;
    QByteArray secretStatic;   // 32 bytes
    QByteArray secretSigning;  // 32 bytes

    bool readFromDisk();
    bool writeToDisk() const;
    void generate();
};

// ─────────────────────────────────────────────────────────────────────────────

bool DevIdentityStore::Impl::readFromDisk()
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;

    const QByteArray blob = file.readAll();
    // 4 x 32-byte fields: static secret/public, signing secret/public.
    if (blob.size() != static_cast<int>(kSecretKeyBytes * 2 + kPublicKeyBytes * 2)) return false;

    secretStatic  = blob.mid(0, kSecretKeyBytes);
    secretSigning = blob.mid(kSecretKeyBytes, kSecretKeyBytes);

    std::memcpy(identity.staticPublicKey.data(), blob.constData() + kSecretKeyBytes * 2,
                kPublicKeyBytes);
    std::memcpy(identity.signingPublicKey.data(),
                blob.constData() + kSecretKeyBytes * 2 + kPublicKeyBytes, kPublicKeyBytes);

    // The PeerId derivation IS the real scheme (AD-05), so handles and
    // identifiers behave exactly as they will with real keys.
    const QByteArray fingerprint = QCryptographicHash::hash(
        QByteArray(reinterpret_cast<const char*>(identity.staticPublicKey.data()),
                   kPublicKeyBytes),
        QCryptographicHash::Sha256);

    identity.peerId = PeerId::fromFingerprint(
        ByteSpan{reinterpret_cast<const Byte*>(fingerprint.constData()),
                 static_cast<std::size_t>(fingerprint.size())});

    loaded = true;
    return true;
}

bool DevIdentityStore::Impl::writeToDisk() const
{
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;

    file.write(secretStatic);
    file.write(secretSigning);
    file.write(reinterpret_cast<const char*>(identity.staticPublicKey.data()), kPublicKeyBytes);
    file.write(reinterpret_cast<const char*>(identity.signingPublicKey.data()), kPublicKeyBytes);
    file.close();

    // Owner-only, which is all a dev build needs; the real store wraps the
    // secret halves with the platform keystore instead.
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

void DevIdentityStore::Impl::generate()
{
    secretStatic  = randomBytes(static_cast<int>(kSecretKeyBytes));
    secretSigning = randomBytes(static_cast<int>(kSecretKeyBytes));

    // With real X25519 the public key is derived from the secret. Here they are
    // independent random values, which is exactly why signatures below are not
    // unforgeable.
    const QByteArray staticPub  = randomBytes(static_cast<int>(kPublicKeyBytes));
    const QByteArray signingPub = randomBytes(static_cast<int>(kPublicKeyBytes));

    std::memcpy(identity.staticPublicKey.data(), staticPub.constData(), kPublicKeyBytes);
    std::memcpy(identity.signingPublicKey.data(), signingPub.constData(), kPublicKeyBytes);

    const QByteArray fingerprint =
        QCryptographicHash::hash(staticPub, QCryptographicHash::Sha256);
    identity.peerId = PeerId::fromFingerprint(
        ByteSpan{reinterpret_cast<const Byte*>(fingerprint.constData()),
                 static_cast<std::size_t>(fingerprint.size())});

    loaded = true;
}

// ─────────────────────────────────────────────────────────────────────────────

DevIdentityStore::DevIdentityStore(QString storagePath) : impl_(std::make_unique<Impl>())
{
    impl_->path = std::move(storagePath);
}

DevIdentityStore::~DevIdentityStore() = default;

bool DevIdentityStore::exists() const
{
    return impl_->loaded || QFile::exists(impl_->path);
}

Outcome<Identity> DevIdentityStore::loadOrCreate()
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

Outcome<Signature> DevIdentityStore::sign(ByteSpan message)
{
    if (!impl_->loaded && !impl_->readFromDisk()) {
        return Outcome<Signature>::error(ErrorCode::PreconditionFailed, "No identity yet.");
    }

    // A keyed hash, NOT a signature: anyone with the public key can produce the
    // same value, because verify() below recomputes it the same way.
    QCryptographicHash hash(QCryptographicHash::Sha512);
    hash.addData(reinterpret_cast<const char*>(impl_->identity.signingPublicKey.data()),
                 kPublicKeyBytes);
    hash.addData(toQt(message));

    const QByteArray digest = hash.result();
    Signature signature{};
    std::memcpy(signature.data(), digest.constData(),
                std::min<std::size_t>(kSignatureBytes, digest.size()));
    return signature;
}

bool DevIdentityStore::verify(ByteSpan message, const Signature& signature,
                              const PublicKey& signingKey)
{
    QCryptographicHash hash(QCryptographicHash::Sha512);
    hash.addData(reinterpret_cast<const char*>(signingKey.data()), kPublicKeyBytes);
    hash.addData(toQt(message));

    const QByteArray digest = hash.result();
    Signature expected{};
    std::memcpy(expected.data(), digest.constData(),
                std::min<std::size_t>(kSignatureBytes, digest.size()));

    return expected == signature;
}

Result DevIdentityStore::wipe()
{
    impl_->loaded = false;
    impl_->secretStatic.fill(0);
    impl_->secretSigning.fill(0);

    if (QFile::exists(impl_->path) && !QFile::remove(impl_->path)) {
        return Result::error(ErrorCode::StorageFailure, "Could not remove the identity file.");
    }
    return Result::ok();
}

}  // namespace anytime::identity
