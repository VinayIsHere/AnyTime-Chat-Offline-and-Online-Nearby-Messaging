// adapters/crypto/NoiseSessionFactory.h
//
// ISecureSession over noise-c (MIT) with a libsodium backend — AD-06.
//
// Pattern: Noise_XX_25519_ChaChaPoly_SHA256
//   · XX gives mutual authentication and forward secrecy
//   · neither side needs the other's static key in advance, which is exactly
//     right for meeting a stranger over BLE
//   · the peer's authenticated static key falls out of the handshake, and that
//     is the value pinned onto a Contact at accept time (§08)
//
// WE ARE NOT DESIGNING CRYPTO HERE. This is a specified protocol implemented
// with vetted primitives. VALIDATE AGAINST THE PUBLISHED NOISE TEST VECTORS
// before trusting it — that test is the whole reason this choice is safe.
//
// Swapping noise-c for a focused in-house XX implementation later touches only
// this file, because everything above depends on ISecureSession.

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

    /// Initialises libsodium. Call once, before any session is created.
    static Result initialiseLibrary();

    Outcome<std::unique_ptr<ISecureSession>> createSession(
        const PublicKey* remoteStatic) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::crypto
