// adapters/identity/KeystoreIdentityStore.h
//
// IIdentityStore over libsodium, with secret material wrapped by the platform
// keystore (Android Keystore over JNI; iOS Keychain in phase 5).
//
// Keys are generated exactly once, on first run. The secret halves are sealed
// immediately and never held in plaintext beyond the generating call, so a
// filesystem dump or an unencrypted device backup yields nothing usable.
//
// DESKTOP: falls back to a file-backed store with a warning, because Windows and
// Linux dev machines have no equivalent hardware-backed store. That fallback is
// for development only and must never be compiled into a release build — the
// CMake guard below enforces it.

#pragma once

#include <QString>

#include <memory>

#include "core/ports/IIdentityStore.h"
#include "adapters/persistence/sqlite/SqliteStore.h"

namespace anytime::identity {

/// Where wrapped key material lives. On Android this is the app's private
/// storage; the wrapping key itself never leaves the Keystore.
struct IdentityConfig {
    QString storagePath;

    /// Development escape hatch. Guarded by ANYTIME_ALLOW_INSECURE_IDENTITY,
    /// which release builds must not define.
    bool allowInsecureFallback = false;
};

class KeystoreIdentityStore final : public IIdentityStore {
public:
    explicit KeystoreIdentityStore(IdentityConfig);
    ~KeystoreIdentityStore() override;

    Outcome<Identity>  loadOrCreate() override;
    bool               exists() const override;
    Outcome<Signature> sign(ByteSpan message) override;
    bool               verify(ByteSpan, const Signature&, const PublicKey&) override;
    Result             wipe() override;

    /// The at-rest data key, handed to SqliteStore. Derived from the same
    /// keystore-wrapped root as the identity, so panic wipe kills both.
    persistence::IDataCipher& dataCipher();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::identity
