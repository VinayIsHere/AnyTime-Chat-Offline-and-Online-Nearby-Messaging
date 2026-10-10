// adapters/crypto/NoiseSessionFactory.h
//
// ISecureSession over Noise_XX_25519_ChaChaPoly_BLAKE2b (AD-06).
//
// Primitives come from vendored Monocypher; the handshake state machine is in
// NoiseProtocol.h. Replaces NullSessionFactory, which failed every call rather
// than pretending to encrypt.
//
// The local static secret never reaches this class. Noise needs DH against our
// static key twice during XX, and both go back through IIdentityStore::agree(),
// so the identity store keeps sole custody of the secret.

#pragma once

#include <memory>

#include "core/ports/ISecureSession.h"

namespace anytime {
class IIdentityStore;
}

namespace anytime::crypto {

class NoiseSessionFactory final : public ISecureSessionFactory {
public:
    explicit NoiseSessionFactory(IIdentityStore&);
    ~NoiseSessionFactory() override;

    /// `remoteStatic` is ignored: XX learns the peer's static key in-band, which
    /// is the whole reason it suits meeting a stranger. A caller that already
    /// knows the key should still verify the pinned value after the handshake.
    Outcome<std::unique_ptr<ISecureSession>> createSession(const PublicKey* remoteStatic) override;

    /// XX message 1 is `-> e` with an empty payload and no key yet, so it is
    /// exactly one 32-byte ephemeral key. Messages 2 (96) and 3 (64) are longer.
    bool isHandshakeInitiation(ByteSpan message) const override;

private:
    IIdentityStore& identity_;
};

}  // namespace anytime::crypto
