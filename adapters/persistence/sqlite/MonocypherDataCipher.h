// adapters/persistence/sqlite/MonocypherDataCipher.h
//
// The at-rest data cipher (docs/ARCHITECTURE.md section 10, section 12).
//
// XChaCha20-Poly1305 from vendored Monocypher, under a 32-byte data key held in
// a file beside the database. `body_encrypted` and `payload_encrypted` in
// SqliteStore hold what this produces, so a device backup or filesystem dump of
// the .db yields nothing readable.
//
// SEALED LAYOUT:  [ nonce : 24 ][ mac : 16 ][ ciphertext : n ]
//
// A 24-byte nonce is the reason this uses the X variant rather than the IETF one
// NoiseProtocol uses: random nonces are only safe at that width. At 12 bytes we
// would need a counter, which means persisting it and getting it right across
// crashes — a real source of catastrophic nonce reuse. Here `crypto_aead_lock`
// takes a fresh random nonce per record and there is no state to keep.
//
// WHAT IS STILL OWED — read this before calling the at-rest story finished.
// The data key sits in a plain file with owner-only permissions, exactly like
// the identity key next to it. That protects against another app reading it and
// against a cloud backup (allowBackup is false), but NOT against someone with
// the unlocked device or root. Wrapping it with the Android Keystore is the
// remaining half of section 10. That work replaces `loadOrCreate()` below and
// touches nothing else — every caller goes through IDataCipher, which is why
// KeystoreIdentityStore::dataCipher() is declared to return this interface.

#pragma once

#include <QString>

#include <memory>

#include "adapters/persistence/sqlite/SqliteStore.h"

namespace anytime::persistence {

class MonocypherDataCipher final : public IDataCipher {
public:
    /// `keyPath` is the data-key file. Created with owner-only permissions on
    /// first run, alongside the identity key.
    explicit MonocypherDataCipher(QString keyPath);
    ~MonocypherDataCipher() override;

    /// Loads the data key, generating one on first run. Call before the store
    /// opens; every seal/open fails loudly until it succeeds.
    Result loadOrCreate();

    /// Panic wipe. Destroys the key, which makes every encrypted row
    /// permanently unreadable — that is the point, not a side effect.
    Result wipe();

    Result seal(ByteSpan plaintext, ByteBuffer& out) override;
    Result open(ByteSpan ciphertext, ByteBuffer& out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::persistence
