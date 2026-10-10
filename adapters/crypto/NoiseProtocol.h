// adapters/crypto/NoiseProtocol.h
//
// Noise_XX_25519_ChaChaPoly_BLAKE2b — the handshake state machine.
//
// WHAT IS HAND-WRITTEN HERE AND WHAT IS NOT. Every cryptographic primitive comes
// from Monocypher: X25519, IETF ChaCha20-Poly1305 (crypto_aead_init_ietf, the
// RFC 8439 construction with a 12-byte nonce) and BLAKE2b. What this file adds is
// the protocol scaffolding the Noise specification defines — HMAC, HKDF, and the
// CipherState / SymmetricState / HandshakeState machines. That is a meaningful
// distinction: getting a state machine wrong is a protocol bug, whereas writing
// a cipher yourself is how you get a timing side channel.
//
// Suite choices:
//   25519       X25519, 32-byte DH output
//   ChaChaPoly  RFC 8439, nonce = 4 zero bytes || little-endian u64 counter
//   BLAKE2b     512-bit, so HASHLEN is 64 and HMAC block length is 128
//
// XX pattern:
//     -> e
//     <- e, ee, s, es
//     -> s, se
//
// XX is the right pattern for meeting a stranger over BLE: neither side needs the
// other's static key in advance, both are mutually authenticated by the end, and
// the peer's authenticated static key falls out of the handshake — which is
// exactly the value pinned onto a Contact when a request is accepted (section 08).

#pragma once

#include <array>
#include <cstdint>

#include "core/domain/Result.h"
#include "core/domain/Types.h"

namespace anytime::crypto::noise {

inline constexpr std::size_t kDhLen    = 32;
inline constexpr std::size_t kHashLen  = 64;   ///< BLAKE2b-512
inline constexpr std::size_t kBlockLen = 128;  ///< BLAKE2b block, for HMAC
inline constexpr std::size_t kKeyLen   = 32;
inline constexpr std::size_t kTagLen   = 16;   ///< Poly1305

using HashBytes = std::array<Byte, kHashLen>;
using KeyBytes  = std::array<Byte, kKeyLen>;
using DhBytes   = std::array<Byte, kDhLen>;

/// Exactly the string the specification hashes into `h`. Changing any part of it
/// changes the protocol, and two peers with different names cannot talk.
inline constexpr const char* kProtocolName = "Noise_XX_25519_ChaChaPoly_BLAKE2b";

// ─────────────────────────────────────────────────────────────────────────────

/// HMAC-BLAKE2b. BLAKE2b has a native keyed mode, but Noise specifies HMAC, and
/// the two are not interchangeable.
void hmac(const Byte* key, std::size_t keyLen, const Byte* data, std::size_t dataLen,
          HashBytes& out);

/// Noise HKDF. `outputs` must be 2 or 3.
void hkdf(const HashBytes& chainingKey, ByteSpan inputKeyMaterial, int outputs,
          HashBytes& out1, HashBytes& out2, HashBytes* out3 = nullptr);

// ─────────────────────────────────────────────────────────────────────────────

class CipherState {
public:
    void initializeKey(const KeyBytes&);
    void clear();
    bool hasKey() const noexcept { return hasKey_; }

    /// Without a key these are the identity function, which is what the spec
    /// requires for the parts of a handshake that precede the first MixKey.
    Result encryptWithAd(ByteSpan ad, ByteSpan plaintext, ByteBuffer& out);
    Result decryptWithAd(ByteSpan ad, ByteSpan ciphertext, ByteBuffer& out);

private:
    KeyBytes      k_{};
    std::uint64_t n_ = 0;
    bool          hasKey_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────

class SymmetricState {
public:
    void initialize(const char* protocolName);

    void mixKey(ByteSpan inputKeyMaterial);
    void mixHash(ByteSpan data);

    Result encryptAndHash(ByteSpan plaintext, ByteBuffer& out);
    Result decryptAndHash(ByteSpan ciphertext, ByteBuffer& out);

    /// Ends the handshake, producing the two transport cipher states.
    void split(CipherState& sending, CipherState& receiving);

    const HashBytes& handshakeHash() const noexcept { return h_; }

private:
    HashBytes   ck_{};
    HashBytes   h_{};
    CipherState cipher_;
};

// ─────────────────────────────────────────────────────────────────────────────

enum class Role { Initiator, Responder };

class HandshakeState {
public:
    /// `staticPublicKey` is ours; the matching secret is reached through the
    /// injected DH callback rather than being copied in, so the identity store
    /// keeps sole custody of it.
    using StaticDh = std::function<bool(const DhBytes& theirPublic, DhBytes& sharedOut)>;

    Result initialize(Role, const KeyBytes& staticPublicKey, StaticDh staticDh);

    /// Writes the next handshake message. `payload` may be empty.
    Result writeMessage(ByteSpan payload, ByteBuffer& out);

    /// Reads the next handshake message, recovering any payload it carried.
    Result readMessage(ByteSpan message, ByteBuffer& payloadOut);

    bool isComplete() const noexcept { return complete_; }

    /// The peer's authenticated static key. Only meaningful once complete.
    bool remoteStaticKey(KeyBytes& out) const;

    /// Transport cipher states, valid once complete.
    CipherState& sendingCipher()   noexcept { return sending_; }
    CipherState& receivingCipher() noexcept { return receiving_; }

private:
    Result writeMessage1(ByteSpan payload, ByteBuffer& out);
    Result writeMessage2(ByteSpan payload, ByteBuffer& out);
    Result writeMessage3(ByteSpan payload, ByteBuffer& out);
    Result readMessage1(ByteSpan, ByteBuffer&);
    Result readMessage2(ByteSpan, ByteBuffer&);
    Result readMessage3(ByteSpan, ByteBuffer&);

    Role           role_ = Role::Initiator;
    SymmetricState symmetric_;
    StaticDh       staticDh_;

    KeyBytes s_pub_{};                 ///< our static public key
    KeyBytes e_pub_{};                 ///< our ephemeral public key
    DhBytes  e_sec_{};                 ///< our ephemeral secret
    KeyBytes rs_{};                    ///< their static
    KeyBytes re_{};                    ///< their ephemeral
    bool     haveRemoteStatic_ = false;

    int  step_ = 0;
    bool complete_ = false;

    CipherState sending_;
    CipherState receiving_;
};

}  // namespace anytime::crypto::noise
