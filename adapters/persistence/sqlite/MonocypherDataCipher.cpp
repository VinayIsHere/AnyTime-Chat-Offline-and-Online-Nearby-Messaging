// adapters/persistence/sqlite/MonocypherDataCipher.cpp
//
// Nothing in this file may log key bytes, plaintext or ciphertext. Lengths and
// outcomes only — a debug log can be pulled off a phone with one adb command.

#include "adapters/persistence/sqlite/MonocypherDataCipher.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>

#include <cstring>

#include "core/log/Log.h"

extern "C" {
#include "third_party/monocypher/monocypher.h"
}

namespace anytime::persistence {

namespace {

constexpr int kDataKeyBytes = 32;
constexpr int kNonceBytes   = 24;  ///< XChaCha20 — wide enough for random nonces
constexpr int kMacBytes     = 16;  ///< Poly1305

/// OS CSPRNG. QRandomGenerator::system(), never ::global() — the latter is fine
/// for jitter and useless for key material. Matches MonocypherIdentityStore.
void randomBytes(std::uint8_t* out, int count)
{
    QByteArray buffer(count, Qt::Uninitialized);
    QRandomGenerator::system()->generate(
        reinterpret_cast<quint32*>(buffer.data()),
        reinterpret_cast<quint32*>(buffer.data() + buffer.size()));
    std::memcpy(out, buffer.constData(), count);
    crypto_wipe(reinterpret_cast<std::uint8_t*>(buffer.data()), count);
}

/// Monocypher reads nothing when the length is zero, but passing a null pointer
/// into a memcpy-shaped API is still undefined. One dummy byte avoids it.
const std::uint8_t kEmpty[1] = {0};

const std::uint8_t* safePtr(const Byte* p) { return p != nullptr ? p : kEmpty; }

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct MonocypherDataCipher::Impl {
    QString      path;
    bool         loaded = false;
    std::uint8_t key[kDataKeyBytes]{};

    ~Impl() { crypto_wipe(key, sizeof key); }
};

MonocypherDataCipher::MonocypherDataCipher(QString keyPath)
    : impl_(std::make_unique<Impl>())
{
    impl_->path = std::move(keyPath);
}

MonocypherDataCipher::~MonocypherDataCipher() = default;

Result MonocypherDataCipher::loadOrCreate()
{
    if (impl_->loaded) return Result::ok();

    QFile file(impl_->path);
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray blob = file.readAll();
        file.close();

        if (blob.size() == kDataKeyBytes) {
            std::memcpy(impl_->key, blob.constData(), kDataKeyBytes);
            crypto_wipe(reinterpret_cast<std::uint8_t*>(blob.data()), blob.size());
            impl_->loaded = true;
            AT_LOG_DEBUG("storage", "at-rest data key loaded");
            return Result::ok();
        }

        // Never silently regenerated: a new key makes every stored message
        // permanently unreadable, which the user would experience as their
        // history vanishing. Fail loudly and let a human decide.
        crypto_wipe(reinterpret_cast<std::uint8_t*>(blob.data()), blob.size());
        AT_LOG_ERROR("storage", "data key file is {} bytes, expected {}", blob.size(),
                     kDataKeyBytes);
        return Result::error(ErrorCode::StorageFailure,
                             "The at-rest data key file is malformed.");
    }

    // First run.
    QDir().mkpath(QFileInfo(impl_->path).absolutePath());
    randomBytes(impl_->key, kDataKeyBytes);

    QFile out(impl_->path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        crypto_wipe(impl_->key, kDataKeyBytes);
        AT_LOG_ERROR("storage", "cannot write the data key file: {}",
                     out.errorString().toStdString());
        return Result::error(ErrorCode::StorageFailure, "Could not write the data key.");
    }
    out.write(reinterpret_cast<const char*>(impl_->key), kDataKeyBytes);
    out.close();
    out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);

    impl_->loaded = true;
    AT_LOG_INFO("storage", "generated a new at-rest data key");
    return Result::ok();
}

Result MonocypherDataCipher::wipe()
{
    AT_LOG_WARN("storage", "wiping the at-rest data key");

    crypto_wipe(impl_->key, kDataKeyBytes);
    impl_->loaded = false;

    if (QFile::exists(impl_->path) && !QFile::remove(impl_->path)) {
        return Result::error(ErrorCode::StorageFailure, "Could not remove the data key.");
    }
    return Result::ok();
}

Result MonocypherDataCipher::seal(ByteSpan plaintext, ByteBuffer& out)
{
    if (!impl_->loaded) {
        return Result::error(ErrorCode::PreconditionFailed, "The at-rest data key is not loaded.");
    }

    out.assign(static_cast<std::size_t>(kNonceBytes + kMacBytes) + plaintext.size(), 0);
    randomBytes(out.data(), kNonceBytes);

    crypto_aead_lock(out.data() + kNonceBytes + kMacBytes,  // ciphertext
                     out.data() + kNonceBytes,              // mac
                     impl_->key,
                     out.data(),                            // nonce
                     nullptr, 0,                            // no associated data
                     safePtr(plaintext.data()), plaintext.size());
    return Result::ok();
}

Result MonocypherDataCipher::open(ByteSpan ciphertext, ByteBuffer& out)
{
    if (!impl_->loaded) {
        return Result::error(ErrorCode::PreconditionFailed, "The at-rest data key is not loaded.");
    }

    const std::size_t overhead = static_cast<std::size_t>(kNonceBytes + kMacBytes);
    if (ciphertext.size() < overhead) {
        return Result::error(ErrorCode::DecryptFailed, "Stored record is too short to be valid.");
    }

    const std::size_t textLen = ciphertext.size() - overhead;
    out.assign(textLen, 0);

    // Returns 0 only when the tag verifies, so this also catches a tampered or
    // truncated row rather than handing back garbage.
    const int ok = crypto_aead_unlock(out.empty() ? nullptr : out.data(),
                                      ciphertext.data() + kNonceBytes,  // mac
                                      impl_->key,
                                      ciphertext.data(),                // nonce
                                      nullptr, 0,
                                      ciphertext.data() + kNonceBytes + kMacBytes, textLen);
    if (ok != 0) {
        out.clear();
        AT_LOG_ERROR("storage", "a stored record failed authentication ({} bytes)",
                     ciphertext.size());
        return Result::error(ErrorCode::DecryptFailed, "A stored record could not be decrypted.");
    }
    return Result::ok();
}

}  // namespace anytime::persistence
