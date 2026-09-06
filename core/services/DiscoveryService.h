// core/services/DiscoveryService.h
//
// The Nearby screen's backing service. Owns peer liveness.
//
// LIVENESS COMES FROM ANNOUNCES, NOT LINK EVENTS (§11). BLE connection callbacks
// are unreliable on every platform — iOS only infers a central connected once it
// reads or writes. So a peer is reachable while a signature-verified ANNOUNCE
// sits inside the 60 s window, and three missed announces mark it lost. Link
// up/down is a resource concern, never a correctness one.

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

    /// POST /{t}/api/discovery — starts scanning, and advertising when the
    /// account is discoverable.
    Result start(DiscoveryMode mode = DiscoveryMode::Active);

    /// DELETE /{t}/api/discovery
    void stop();

    bool isRunning() const { return running_; }

    /// GET /{t}/api/discovery/peers — peers inside the reachability window,
    /// strongest signal first. Blocked peers are excluded.
    Outcome<std::vector<Peer>> reachablePeers();

    /// Called by MeshEngine for every verified ANNOUNCE. Upserts the peer and
    /// emits peer.discovered on first sight.
    void onAnnounce(const PeerId&, const std::string& nickname,
                    const PublicKey&, int rssi);

    /// Resolves a transport handle for a peer we want to reach. Returns an error
    /// when the peer has aged out of the window.
    Outcome<PeerHandle> handleFor(const PeerId&) const;

    /// Records where a peer was last seen, so a link can be opened without
    /// rescanning. Called by MeshEngine for every onPeerSeen.
    void rememberHandle(const PeerHandle&);

    static SignalBand bandForRssi(int rssi);

private:
    /// Timer-driven: emits peer.lost for anything past the window.
    void sweepStalePeers();

    TransportRegistry&  registry_;
    MeshEngine&         mesh_;
    IContactRepository& contacts_;
    IEventSink&         events_;
    IClock&             clock_;

    bool          running_ = false;
    DiscoveryMode mode_    = DiscoveryMode::Off;
    TimerId       sweepTimer_ = kInvalidTimer;

    /// Last handle seen per peer, so we can open a link without rescanning.
    std::unordered_map<PeerId, PeerHandle> handles_;

    /// Peers the UI currently believes are present. Guards against emitting
    /// peer.discovered on every announce, or peer.lost more than once.
    std::unordered_set<PeerId> announced_;
};

}  // namespace anytime
