// core/ports/ISecureSession.h
//
// Noise XX sessions (AD-06). Implemented in adapters/crypto over noise-c (MIT)
// with a libsodium backend.
//
// CRITICAL PROPERTY (§06): a session binds to the PEER, not to the link. You
// handshake once over BLE; the resulting cipher state encrypts a video sent over
// Nearby Connections minutes later. No second handshake, no second trust
// decision. SessionManager therefore keys sessions on PeerId — never on LinkId.

#pragma once

#include <memory>

#include "core/domain/Result.h"
#include "core/domain/Types.h"

namespace anytime {

/// One Noise XX session with one peer.
///
/// Handshake flow (three messages):
///   initiator: beginHandshake() -> msg1 ------------------> responder: readHandshake(msg1)
///   initiator: readHandshake(msg2) <-- msg2 <-------------- responder: writeHandshake() -> msg2
///   initiator: writeHandshake() -> msg3 ------------------> responder: readHandshake(msg3)
///   both: isEstablished() == true, remoteStaticKey() available
class ISecureSession {
public:
    virtual ~ISecureSession() = default;

    /// Starts the handshake. For an initiator, `out` receives message 1.
    /// For a responder, `out` is left empty — call readHandshake() first.
    virtual Result beginHandshake(HandshakeRole, ByteBuffer& out) = 0;

    /// Consumes one inbound handshake message. If a reply is required it is
    /// written to `out`; otherwise `out` is left empty.
    virtual Result readHandshake(ByteSpan message, ByteBuffer& out) = 0;

    virtual bool isEstablished() const = 0;

    /// The peer's authenticated Curve25519 static key. Only valid once
    /// isEstablished(). This is what gets pinned on accept (§08).
    virtual Outcome<PublicKey> remoteStaticKey() const = 0;

    virtual Result encrypt(ByteSpan plaintext, ByteBuffer& out) = 0;
    virtual Result decrypt(ByteSpan ciphertext, ByteBuffer& out) = 0;
};

/// Creates sessions and owns the local static keypair.
class ISecureSessionFactory {
public:
    virtual ~ISecureSessionFactory() = default;

    /// `remoteStatic` is supplied when we already know the peer's key (a pinned
    /// contact); pass nullptr for a first contact, where XX learns it in-band.
    virtual Outcome<std::unique_ptr<ISecureSession>> createSession(
        const PublicKey* remoteStatic) = 0;

    /// True when `message` opens a handshake — what a peer sends when it starts
    /// over. SessionManager uses it to spot a peer that restarted (and lost its
    /// session) and two peers that started at the same moment, without having
    /// to know which handshake pattern is in use.
    virtual bool isHandshakeInitiation(ByteSpan message) const = 0;
};

}  // namespace anytime
