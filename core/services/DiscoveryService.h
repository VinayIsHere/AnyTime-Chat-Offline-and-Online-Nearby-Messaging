// core/services/DiscoveryService.h
//
// The Nearby screen's backing service. Owns peer liveness, and decides when to
// open links.
//
// LIVENESS COMES FROM ANNOUNCES, NOT LINK EVENTS (§11). BLE connection callbacks
// are unreliable on every platform — iOS only infers a central connected once it
// reads or writes. So a peer is reachable while a signature-verified ANNOUNCE
// sits inside the 60 s window, and three missed announces mark it lost. Link
// up/down is a resource concern, never a correctness one.
//
// LINK POLICY LIVES HERE, NOT IN TRANSPORTS. A transport reports that it saw a
// peer; the core decides whether to connect. Until this moved, the loopback
// transport connected by itself the moment it saw anyone — so desktop testing
// passed while BLE, which rightly does not auto-connect, would never have opened
// a link at all. Both transports now go through the same decision, which is the
// whole point of AD-07.

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/domain/Entities.h"
#include "core/domain/Result.h"
#include "core/ports/IClock.h"
#include "core/ports/ITransport.h"

namespace anytime {

class IContactRepository;
class IEventSink;
class IClock;
class MeshEngine;
class TransportRegistry;

class DiscoveryService {
public:
    DiscoveryService(TransportRegistry&, MeshEngine&, IContactRepository&,
                     IEventSink&, IClock&);
    ~DiscoveryService();

    /// Set by AppCore once an account exists. The link tie-break compares it.
    void setLocalPeerId(const PeerId&);

    /// POST /{t}/api/discovery — starts scanning. The requested mode is
    /// remembered even when no transport is ready yet, so resume() can finish the
    /// job without the UI asking again.
    Result start(DiscoveryMode mode = DiscoveryMode::Active);

    /// DELETE /{t}/api/discovery
    void stop();

    /// Re-applies the last requested discovery. AppCore calls it whenever a
    /// transport comes up — Bluetooth switched on, or permission granted while
    /// the Nearby screen was already open.
    void resume();

    bool isRunning() const { return running_; }

    /// GET /{t}/api/discovery/peers — peers inside the reachability window,
    /// strongest signal first. Blocked peers are excluded.
    Outcome<std::vector<Peer>> reachablePeers();

    /// Called by MeshEngine for every verified ANNOUNCE. Upserts the peer and
    /// emits peer.discovered on first sight.
    void onAnnounce(const PeerId&, const std::string& nickname,
                    const PublicKey&, int rssi);

    /// Every raw sighting from a transport: remembers where the peer is, and
    /// decides whether to open a link. See maybeOpenLink() for the tie-break.
    void onPeerSeen(TransportId, const PeerHandle&);

    /// Resolves a transport handle for a peer we want to reach. Returns an error
    /// when the peer has aged out of the window.
    Outcome<PeerHandle> handleFor(const PeerId&) const;

    static SignalBand bandForRssi(int rssi);

private:
    /// Timer-driven: emits peer.lost for anything past the window.
    void sweepStalePeers();

    void maybeOpenLink(TransportId, const PeerHandle&);

    TransportRegistry&  registry_;
    MeshEngine&         mesh_;
    IContactRepository& contacts_;
    IEventSink&         events_;
    IClock&             clock_;

    PeerId        localPeerId_;
    bool          running_     = false;
    DiscoveryMode mode_        = DiscoveryMode::Off;
    DiscoveryMode desiredMode_ = DiscoveryMode::Off;
    TimerId       sweepTimer_  = kInvalidTimer;

    /// Last handle seen per peer, so we can open a link without rescanning.
    std::unordered_map<PeerId, PeerHandle> handles_;

    /// Peers the UI currently believes are present. Guards against emitting
    /// peer.discovered on every announce, or peer.lost more than once.
    std::unordered_set<PeerId> announced_;

    /// When each peer was first seen without a link — starts the grace period
    /// after which the higher-id side connects on its own.
    std::unordered_map<PeerId, Timestamp> firstSeenUnlinked_;

    /// Last connection attempt per peer, for kLinkRetryMillis.
    std::unordered_map<PeerId, Timestamp> lastLinkAttempt_;
};

}  // namespace anytime
