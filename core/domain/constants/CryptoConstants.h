// core/domain/constants/CryptoConstants.h
//
// Key and signature geometry. These are properties of the algorithms in AD-06
// (Curve25519 / Ed25519 / ChaCha20-Poly1305), not tuning knobs — changing one
// means changing the cipher suite.

#pragma once

#include <cstddef>

namespace anytime {

/// First 8 bytes of the SHA-256 fingerprint of the Curve25519 static public key.
/// This is the identity on the wire and in the database (AD-05).
inline constexpr std::size_t kPeerIdBytes = 8;

/// Full fingerprint length, before truncation to a peer id.
inline constexpr std::size_t kFingerprintBytes = 32;

inline constexpr std::size_t kPublicKeyBytes = 32;  ///< X25519 and Ed25519
inline constexpr std::size_t kSecretKeyBytes = 32;
inline constexpr std::size_t kSignatureBytes = 64;  ///< Ed25519

/// Characters of the peer id hex shown as a display discriminator — the "4f2a"
/// in "@vinay·4f2a".
///
/// NOT A SECURITY PROPERTY. Four hex characters is 16 bits; an attacker can
/// grind keypairs to match a chosen suffix in well under a second. It exists to
/// separate honest username collisions, nothing more. Real verification needs
/// the full fingerprint compared out of band — a phase 4 feature.
inline constexpr std::size_t kShortLabelChars = 4;

}  // namespace anytime
