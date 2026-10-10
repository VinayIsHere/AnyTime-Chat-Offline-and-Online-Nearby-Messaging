// core/services/TransportRegistry.h
//
// Owns every registered ITransport and is the single ITransportListener the
// core installs. Transports never talk to services directly — everything funnels
// through here and then into MeshEngine.
//
// This is the class that makes phase 5 cheap: registering NearbyConnections is
// one addTransport() call, and RoutingPolicy picks it up because it advertises
// supportsBulk.

#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "core/domain/Result.h"
#include "core/ports/ITransport.h"

namespace anytime {

/// One logical peer may hold links on several transports at once (§06). This is
/// what lets a Noise session opened over BLE encrypt bulk sent over Wi-Fi later.
struct PeerLinks {
    PeerId                                    peerId;
    std::unordered_map<TransportId, LinkId>   links;  ///< the link routing uses, per transport

    bool   hasLink(TransportId t) const { return links.count(t) > 0; }
    LinkId linkFor(TransportId t) const;
};

class TransportRegistry : public ITransportListener {
public:
    TransportRegistry();
    ~TransportRegistry() override;

    /// Takes ownership. Call before start(). Registration order does not matter;
    /// RoutingPolicy selects on capabilities, never on order.
    void addTransport(std::unique_ptr<ITransport>);

    /// Downstream consumer — MeshEngine. Set exactly once, before start().
    void setListener(ITransportListener*);

    /// Starts every transport. Safe to call repeatedly — transports ignore a
    /// second start — which is what AppCore's retry timer relies on. Succeeds
    /// when at least one transport is running.
    Result startAll(const PeerId& localPeerId);
    void   stopAll();

    /// True when every registered transport reports itself usable.
    bool allAvailable() const;

    Result startDiscovery(DiscoveryMode);
    void   stopDiscovery();
    Result setAdvertising(bool enabled);

    ITransport* find(TransportId) const;
    std::vector<ITransport*> available() const;

    /// Opens a link on the given transport if one is not already open.
    Outcome<LinkId> ensureLink(TransportId, const PeerHandle&);

    const PeerLinks* linksFor(const PeerId&) const;

    /// Attaches an incoming link to the peer on the other end.
    ///
    /// A BLE peripheral learns nothing about a central until it speaks, so an
    /// incoming link comes up with a null PeerId and cannot carry directed
    /// traffic — no handshake reply, no message. MeshEngine calls this when a
    /// signature-verified, un-relayed ANNOUNCE arrives on such a link.
    ///
    /// Only UNBOUND links are bound. A link that is already identified is never
    /// re-pointed, so a replayed announce cannot redirect a known peer's traffic.
    bool bindLink(TransportId, LinkId, const PeerId&);

    // ── ITransportListener — records link state, then fans out ───────────────
    void onPeerSeen(TransportId, const PeerHandle&) override;
    void onLinkUp(TransportId, LinkId, const PeerId&) override;
    void onLinkDown(TransportId, LinkId, LinkCloseReason) override;
    void onFrame(TransportId, LinkId, ByteSpan) override;
    void onTransportStateChanged(TransportId, bool available) override;

private:
    void attach(TransportId, LinkId, const PeerId&);

    std::vector<std::unique_ptr<ITransport>> transports_;
    ITransportListener*                      downstream_ = nullptr;

    std::unordered_map<PeerId, PeerLinks> peerLinks_;

    /// Every identified link, so onLinkDown finds its peer without a scan and can
    /// promote a spare link on the same transport when the preferred one drops.
    std::unordered_map<LinkId, PeerId>      linkOwners_;
    std::unordered_map<LinkId, TransportId> linkTransports_;

    /// Links that are up but not yet identified — see bindLink().
    std::unordered_map<LinkId, TransportId> unboundLinks_;
};

}  // namespace anytime
