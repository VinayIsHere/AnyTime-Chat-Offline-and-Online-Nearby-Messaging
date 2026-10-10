// core/services/SessionManager.h
//
// Owns one ISecureSession per peer and drives the Noise XX handshake.
//
// SESSIONS KEY ON PeerId, NEVER ON LinkId (§06). A session survives its link
// closing and reopening, and a session established over BLE is reused verbatim
// for bulk sent over another transport in phase 5. Keying on links would silently
// break that and force a redundant handshake — and a second trust decision.
//
// Three situations a lossy radio makes routine, and that this class handles:
//
//   STALLED HANDSHAKES. One lost handshake packet leaves a session
//   "Handshaking" forever, and every envelope queued behind it. expireIfStale()
//   drops a handshake older than kHandshakeTimeoutMillis so it can start over.
//
//   CROSSED HANDSHAKES. Both people tap "say hi" at once, so both sides are
//   initiators holding each other's first message. Left alone, each rejects the
//   other's and both retry — at the same moment again. The lower peer id keeps
//   the initiator role; the higher one yields and answers.
//
//   RESTARTED PEERS. Sessions live in memory, so a peer that restarts the app
//   opens a fresh handshake against a session we still think is established.
//   A new first message replaces the old session instead of being rejected.

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

    /// Resolves crossed handshakes deterministically. Set by AppCore once an
    /// account exists.
    void setLocalPeerId(const PeerId&);

    SessionState stateFor(const PeerId&) const;

    /// Starts an outbound handshake, producing message 1 in `out`. No-op returning
    /// Ok if a session is already established or in flight.
    Result beginHandshake(const PeerId&, ByteBuffer& out);

    /// Feeds an inbound NOISE_HANDSHAKE payload. Creates a responder session on
    /// first sight. `out` receives the reply when one is due.
    Result readHandshake(const PeerId&, ByteSpan message, ByteBuffer& out);

    /// Drops a handshake that failed or has been in flight too long. Returns
    /// true when something was dropped, so the caller can start over.
    bool expireIfStale(const PeerId&);

    /// The peer's authenticated static key, once established. This is the value
    /// pinned onto a Contact at accept time (§08).
    Outcome<PublicKey> remoteStaticKey(const PeerId&) const;

    Result encrypt(const PeerId&, ByteSpan plaintext, ByteBuffer& out);
    Result decrypt(const PeerId&, ByteSpan ciphertext, ByteBuffer& out);

    /// Drops a session — on panic wipe, on block, or after a failure.
    void   forget(const PeerId&);
    void   forgetAll();

private:
    struct Entry {
        std::unique_ptr<ISecureSession> session;
        SessionState                    state     = SessionState::None;
        HandshakeRole                   role      = HandshakeRole::Initiator;
        Timestamp                       startedAt = kNoTimestamp;
    };

    ISecureSessionFactory& factory_;
    IClock&                clock_;
    PeerId                 localPeerId_;

    std::unordered_map<PeerId, Entry> sessions_;
};

}  // namespace anytime
