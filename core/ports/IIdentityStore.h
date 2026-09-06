// core/ports/IIdentityStore.h
//
// Long-term keypairs (AD-05). Implemented in adapters/identity — on Android the
// secret keys are wrapped by the Android Keystore over JNI, so a filesystem dump
// or device backup yields nothing usable.

#pragma once

#include "core/domain/Result.h"
#include "core/domain/Types.h"

namespace anytime {

// Identity lives in core/domain/GlobalStructures.h.

class IIdentityStore {
public:
    virtual ~IIdentityStore() = default;

    /// Returns the existing identity, generating one on first run. Generation is
    /// the only moment secret material is created; it is wrapped before storage.
    virtual Outcome<Identity> loadOrCreate() = 0;

    virtual bool exists() const = 0;

    /// Signs with the Ed25519 key. Used for ANNOUNCE packets only.
    virtual Outcome<Signature> sign(ByteSpan message) = 0;

    /// Verifies a peer's announce signature against their claimed signing key.
    virtual bool verify(ByteSpan message, const Signature&, const PublicKey& signingKey) = 0;

    /// Panic wipe — destroys the identity and the at-rest data key. Irreversible.
    virtual Result wipe() = 0;
};

}  // namespace anytime
