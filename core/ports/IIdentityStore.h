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

    /// X25519 shared secret between our static key and theirs.
    ///
    /// This is how the Noise handshake performs its two static DH operations
    /// without the secret key ever leaving the store. Implementations MUST
    /// reject a degenerate (all-zero) result, which indicates a low-order peer
    /// key rather than a real one.
    virtual Outcome<std::array<Byte, 32>> agree(const PublicKey& theirStatic) = 0;

    /// The PeerId a static public key maps to: SHA-256(key)[0..8] (AD-05).
    ///
    /// The sender field of a packet is just bytes anyone can write. This is how
    /// the core checks that an announce, or a completed handshake, really
    /// belongs to the peer id it claims. Without the check a spoofed announce
    /// could claim someone else's link, or a handshake could put an attacker's
    /// key behind a contact's id.
    virtual PeerId peerIdFor(const PublicKey& staticKey) const = 0;

    /// Panic wipe — destroys the identity and the at-rest data key. Irreversible.
    virtual Result wipe() = 0;
};

}  // namespace anytime
