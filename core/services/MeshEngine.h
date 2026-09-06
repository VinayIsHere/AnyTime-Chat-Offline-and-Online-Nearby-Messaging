// core/services/MeshEngine.h
//
// The protocol loop. Sits between TransportRegistry and the application services.
//
// INBOUND:  frame → reassemble → decode → dedup → verify/decrypt → dispatch
// OUTBOUND: envelope → encrypt → encode → fragment → route → send
//
// Everything bitchat's whitepaper describes as "the mesh" lives here: TTL
// handling, the dedup cache, announce scheduling and relay policy. None of it is
// in the platform radio layer, which is why the iOS port does not have to
// reimplement any of it (§11).

#pragma once

#include <functional>
#include <memory>
#include <unordered_map>

#include "core/domain/Entities.h"
#include "core/domain/Result.h"
#include "core/ports/ITransport.h"
#include "core/protocol/AppEnvelope.h"
#include "core/protocol/DedupCache.h"
#include "core/protocol/Fragmenter.h"
#include "core/protocol/Packet.h"

namespace anytime {

class IClock;
class IIdentityStore;
class RoutingPolicy;
class SessionManager;
class TransportRegistry;

/// What MeshEngine hands upward once a packet is authenticated and decrypted.
class IMeshListener {
public:
    virtual ~IMeshListener() = default;

    /// A signature-verified ANNOUNCE. Drives peer liveness (§11).
    virtual void onAnnounce(const PeerId&, const std::string& nickname,
                            const PublicKey& staticKey, int rssi) = 0;

    /// A decrypted application envelope from an established session.
    virtual void onEnvelope(const PeerId&, const protocol::AppEnvelope&) = 0;

    /// Peer acknowledged one of our messages.
    virtual void onAck(const PeerId&, const ClientMessageId&) = 0;

    /// A Noise session reached Established — queued sends for this peer can go.
    virtual void onSessionEstablished(const PeerId&) = 0;
};

class MeshEngine : public ITransportListener {
public:
    MeshEngine(TransportRegistry&, SessionManager&, RoutingPolicy&,
               IIdentityStore&, protocol::ICborCodec&, IClock&);
    ~MeshEngine() override;

    void setListener(IMeshListener*);

    /// Begins announce scheduling: every 4 s while isolated, backing off to a
    /// jittered 15–30 s once peers are known (§07).
    Result start(const Account&);
    void   stop();

    /// Sets the nickname and discoverability carried in future announces.
    void updateAnnounceContent(const std::string& nickname, bool discoverable);

    /// Encrypts, frames, fragments and sends. Fails when no session or no
    /// transport is available — the caller queues to the outbox in that case.
    Result sendEnvelope(const PeerId&, const protocol::AppEnvelope&, PayloadClass);

    /// Fire-and-forget delivery acknowledgement.
    Result sendAck(const PeerId&, const ClientMessageId&);

    /// Ensures a Noise session exists, starting a handshake if needed.
    Result ensureSession(const PeerId&);

    // ── ITransportListener ───────────────────────────────────────────────────
    void onPeerSeen(TransportId, const PeerHandle&) override;
    void onLinkUp(TransportId, LinkId, const PeerId&) override;
    void onLinkDown(TransportId, LinkId, LinkCloseReason) override;
    void onFrame(TransportId, LinkId, ByteSpan) override;
    void onTransportStateChanged(TransportId, bool available) override;

    /// DiscoveryService installs these so it learns about raw transport events
    /// without MeshEngine depending on it — keeps the dependency one-way.
    void setPeerSeenHook(std::function<void(TransportId, const PeerHandle&)> hook) {
        peerSeenHook_ = std::move(hook);
    }
    void setRadioStateHook(std::function<void(bool)> hook) {
        radioStateHook_ = std::move(hook);
    }

private:
    void handlePacket(TransportId, LinkId, const protocol::Packet&, int rssi);
    void handleAnnounce(const protocol::Packet&, int rssi);
    void handleHandshake(const protocol::Packet&);
    void handleNoiseMessage(const protocol::Packet&);
    void handleAck(const protocol::Packet&);

    /// AD-09 — fields exist from phase 1; the behaviour is behind this flag.
    void maybeRelay(const protocol::Packet&, TransportId arrivedOn);

    void scheduleAnnounce();
    void emitAnnounce();
    void broadcastFrame(const ByteBuffer&);

    TransportRegistry&    registry_;
    SessionManager&       sessions_;
    RoutingPolicy&        routing_;
    IIdentityStore&       identity_;
    protocol::ICborCodec& cbor_;
    IClock&               clock_;

    IMeshListener* listener_ = nullptr;

    protocol::DedupCache  dedup_;
    protocol::Reassembler reassembler_;

    PeerId      localPeerId_;
    std::string nickname_;
    PublicKey   staticKey_{};
    PublicKey   signingKey_{};
    bool        discoverable_ = true;
    bool        relayEnabled_ = false;  ///< AD-09, off for v1
    bool        running_      = false;

    TimerId announceTimer_  = kInvalidTimer;
    int     knownPeerCount_ = 0;

    /// Most recent RSSI per peer, so an announce can be banded even though the
    /// signal arrives on a different callback than the packet.
    std::unordered_map<PeerId, int> lastRssi_;

    std::function<void(TransportId, const PeerHandle&)> peerSeenHook_;
    std::function<void(bool)>                          radioStateHook_;
};

}  // namespace anytime
