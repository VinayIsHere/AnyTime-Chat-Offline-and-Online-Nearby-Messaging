// core/services/SessionManager.h
//
// Owns one ISecureSession per peer and drives the Noise XX handshake.
//
// SESSIONS KEY ON PeerId, NEVER ON LinkId (§06). A session survives its link
// closing and reopening, and a session established over BLE is reused verbatim
// for bulk sent over another transport in phase 5. Keying on links would silently
// break that and force a redundant handshake — and a second trust decision.

#pragma once

#include <memory>
#include <unordered_map>

#include "core/domain/Result.h"
#include "core/domain/Types.h"
#include "core/ports/ISecureSession.h"

namespace anytime {

class IClock;

class SessionManager {
public:
    SessionManager(ISecureSessionFactory&, IClock&);
    ~SessionManager();

    SessionState stateFor(const PeerId&) const;

    /// Starts an outbound handshake, producing message 1 in `out`. No-op returning
    /// Ok if a session is already established or in flight.
    Result beginHandshake(const PeerId&, ByteBuffer& out);

    /// Feeds an inbound NOISE_HANDSHAKE payload. Creates a responder session on
    /// first sight. `out` receives the reply when one is due.
    Result readHandshake(const PeerId&, ByteSpan message, ByteBuffer& out);

    /// The peer's authenticated static key, once established. This is the value
    /// pinned onto a Contact at accept time (§08).
    Outcome<PublicKey> remoteStaticKey(const PeerId&) const;

    Result encrypt(const PeerId&, ByteSpan plaintext, ByteBuffer& out);
    Result decrypt(const PeerId&, ByteSpan ciphertext, ByteBuffer& out);

    /// Drops a session — on panic wipe, on block, or after repeated failures.
    void   forget(const PeerId&);
    void   forgetAll();

private:
    struct Entry {
        std::unique_ptr<ISecureSession> session;
        SessionState                    state = SessionState::None;
        Timestamp                       startedAt = kNoTimestamp;
    };

    ISecureSessionFactory& factory_;
    IClock&                clock_;

    std::unordered_map<PeerId, Entry> sessions_;
};

}  // namespace anytime
