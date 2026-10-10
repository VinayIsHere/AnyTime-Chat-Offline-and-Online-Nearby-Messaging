// adapters/identity/MonocypherIdentityStore.h
//
// REAL cryptographic identity, replacing DevIdentityStore.
//
//   · X25519 keypair — the static key. Its SHA-256 fingerprint, truncated to
//     8 bytes, IS the PeerId (AD-05).
//   · Ed25519 keypair — signs ANNOUNCE packets. RFC 8032 standard variant, so a
//     future iOS build using CryptoKit interoperates without translation.
//
// Primitives come from Monocypher 4.0.2 (third_party/monocypher), dual-licensed
// BSD-2-Clause / CC-0. Vendored as four files rather than pulled as a dependency
// because cross-compiling a crypto library for the NDK is its own project, and
// Monocypher is explicitly designed to be dropped into a source tree.
//
// WHAT THIS FIXES: DevIdentityStore's "signatures" were a keyed hash that anyone
// holding the public key could forge, so announce authenticity was decorative.
// These are real Ed25519 signatures — a forged one is rejected.
//
// WHAT IS STILL OWED: secret keys are stored unencrypted. Wrapping them with the
// Android Keystore is the remaining half of section 10, and it changes only this
// file. See docs/RUNNING.md.

#pragma once

#include <QString>

#include <memory>

#include "core/ports/IIdentityStore.h"

namespace anytime::identity {

class MonocypherIdentityStore final : public IIdentityStore {
public:
    /// `storagePath` is the key file. Created with owner-only permissions.
    explicit MonocypherIdentityStore(QString storagePath);
    ~MonocypherIdentityStore() override;

    Outcome<Identity>  loadOrCreate() override;
    bool               exists() const override;
    Outcome<Signature> sign(ByteSpan message) override;
    bool               verify(ByteSpan, const Signature&, const PublicKey&) override;
    Result             wipe() override;

    Outcome<std::array<Byte, 32>> agree(const PublicKey& theirStatic) override;
    PeerId                        peerIdFor(const PublicKey& staticKey) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::identity
