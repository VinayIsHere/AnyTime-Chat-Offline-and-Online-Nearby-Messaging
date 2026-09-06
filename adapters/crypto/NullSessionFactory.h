// adapters/crypto/NullSessionFactory.h
//
// Placeholder ISecureSessionFactory for the sign-in and Nearby milestone.
//
// Every call fails with a clear error rather than pretending to encrypt. That is
// deliberate: a factory that returned a passthrough "session" would let
// plaintext flow while looking encrypted, which is the single worst failure mode
// available to us. Loud failure beats silent insecurity.
//
// Discovery does not need sessions — announces are signed and cleartext by
// design — so this is enough to run Nearby end to end. It is replaced by
// NoiseSessionFactory (AD-06) in the messaging milestone.

#pragma once

#include "core/ports/ISecureSession.h"

namespace anytime::crypto {

class NullSessionFactory final : public ISecureSessionFactory {
public:
    Outcome<std::unique_ptr<ISecureSession>> createSession(const PublicKey*) override
    {
        return Outcome<std::unique_ptr<ISecureSession>>::error(
            ErrorCode::HandshakeFailed,
            "Encrypted sessions arrive with the messaging milestone (AD-06).");
    }
};

}  // namespace anytime::crypto
