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
    std::unordered_map<TransportId, LinkId>   links;

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

    Result startAll(const PeerId& localPeerId);
    void   stopAll();

    Result startDiscovery(DiscoveryMode);
    void   stopDiscovery();
    Result setAdvertising(bool enabled);

    ITransport* find(TransportId) const;
    std::vector<ITransport*> available() const;

    /// Opens a link on the given transport if one is not already open.
    Outcome<LinkId> ensureLink(TransportId, const PeerHandle&);

    const PeerLinks* linksFor(const PeerId&) const;

    // ── ITransportListener — fans out to the downstream listener ─────────────
    void onPeerSeen(TransportId, const PeerHandle&) override;
    void onLinkUp(TransportId, LinkId, const PeerId&) override;
    void onLinkDown(TransportId, LinkId, LinkCloseReason) override;
    void onFrame(TransportId, LinkId, ByteSpan) override;
    void onTransportStateChanged(TransportId, bool available) override;

private:
    std::vector<std::unique_ptr<ITransport>> transports_;
    ITransportListener*                      downstream_ = nullptr;

    std::unordered_map<PeerId, PeerLinks> peerLinks_;
    /// Reverse index so onLinkDown can find the peer without a scan.
    std::unordered_map<LinkId, PeerId>    linkOwners_;
};

}  // namespace anytime
