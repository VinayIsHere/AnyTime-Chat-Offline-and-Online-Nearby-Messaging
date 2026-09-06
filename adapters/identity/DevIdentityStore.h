// adapters/identity/DevIdentityStore.h
//
// ############################################################################
// #                                                                          #
// #   THIS IS NOT REAL CRYPTOGRAPHY. IT MUST NOT SHIP.                        #
// #                                                                          #
// ############################################################################
//
// A development stand-in for IIdentityStore so the sign-in and Nearby flows can
// be built and tested before the crypto library lands (AD-06).
//
// What it does:
//   · generates random bytes and calls them a keypair
//   · derives the PeerId as SHA-256(publicKey) truncated to 8 bytes — this part
//     IS the real scheme, so handles and identifiers behave correctly
//   · "signs" by hashing the message with the secret key, and "verifies" by
//     recomputing that hash
//
// What that means:
//   · signatures are NOT unforgeable — anyone holding the public key can mint
//     one, so announce authenticity is currently decorative
//   · there is NO key agreement, so no Noise session can be established
//   · keys are stored unencrypted on disk
//
// It exists because the alternative — stubbing identity out entirely — would
// have left PeerId, handles, the fingerprint discriminator and the whole
// announce path untested until the crypto milestone. Everything above this
// interface is real and stays unchanged when libsodium replaces it.
//
// A release build that includes this file fails to compile, deliberately.

#pragma once

#include <QString>

#include <memory>

#include "core/ports/IIdentityStore.h"

#if defined(ANYTIME_FORBID_INSECURE_IDENTITY)
#error "DevIdentityStore must never be compiled into a release build. \
Replace it with the libsodium-backed KeystoreIdentityStore (AD-06)."
#endif

namespace anytime::identity {

class DevIdentityStore final : public IIdentityStore {
public:
    /// `storagePath` is a plain file holding the generated key material.
    explicit DevIdentityStore(QString storagePath);
    ~DevIdentityStore() override;

    Outcome<Identity>  loadOrCreate() override;
    bool               exists() const override;
    Outcome<Signature> sign(ByteSpan message) override;
    bool               verify(ByteSpan, const Signature&, const PublicKey&) override;
    Result             wipe() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace anytime::identity
