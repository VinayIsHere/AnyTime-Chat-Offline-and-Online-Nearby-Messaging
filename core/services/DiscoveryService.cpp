// core/services/DiscoveryService.cpp

#include "core/services/DiscoveryService.h"

#include <algorithm>

#include "core/ports/IEventSink.h"
#include "core/ports/IRepositories.h"
#include "core/services/MeshEngine.h"
#include "core/services/TransportRegistry.h"

namespace anytime {

DiscoveryService::DiscoveryService(TransportRegistry& registry, MeshEngine& mesh,
                                   IContactRepository& contacts, IEventSink& events,
                                   IClock& clock)
    : registry_(registry), mesh_(mesh), contacts_(contacts), events_(events), clock_(clock)
{
}

DiscoveryService::~DiscoveryService()
{
    if (sweepTimer_ != kInvalidTimer) clock_.cancel(sweepTimer_);
}

SignalBand DiscoveryService::bandForRssi(int rssi)
{
    if (rssi == 0) return SignalBand::Unknown;
    if (rssi >= kRssiVeryStrongDbm) return SignalBand::VeryStrong;
    if (rssi >= kRssiStrongDbm) return SignalBand::Strong;
    return SignalBand::Weak;
}

Result DiscoveryService::start(DiscoveryMode mode)
{
    if (running_ && mode == mode_) return Result::ok();

    const Result r = registry_.startDiscovery(mode);
    if (r.isError()) {
        events_.onRadioStateChanged(false, "no_transport");
        return r;
    }

    running_ = true;
    mode_ = mode;

    if (sweepTimer_ == kInvalidTimer) {
        sweepTimer_ = clock_.scheduleRepeating(kPeerSweepIntervalMillis,
                                               [this] { sweepStalePeers(); });
    }

    events_.onRadioStateChanged(true, "");
    return Result::ok();
}

void DiscoveryService::stop()
{
    if (!running_) return;

    registry_.stopDiscovery();
    running_ = false;
    mode_ = DiscoveryMode::Off;

    if (sweepTimer_ != kInvalidTimer) {
        clock_.cancel(sweepTimer_);
        sweepTimer_ = kInvalidTimer;
    }
}

void DiscoveryService::onAnnounce(const PeerId& peerId, const std::string& nickname,
                                  const PublicKey& staticKey, int rssi)
{
    const Timestamp now = clock_.now();

    // A blocked peer is dropped before it can reach the UI at all. Blocking is
    // keyed on the fingerprint, so renaming does not evade it (section 08).
    if (contacts_.isBlocked(peerId)) return;

    const bool isNew = announced_.find(peerId) == announced_.end();

    Peer peer;
    peer.peerId = peerId;
    peer.staticPublicKey = staticKey;
    peer.nickname = nickname;
    peer.signal = bandForRssi(rssi);
    peer.verifiedAnnounceAt = now;
    peer.lastSeenAt = now;

    contacts_.upsertPeer(peer);

    // Only announce arrival once. Subsequent announces refresh the window
    // silently, otherwise the Nearby list would churn every few seconds.
    if (isNew) {
        announced_.insert(peerId);
        events_.onPeerDiscovered(peer);
    }
}

Outcome<std::vector<Peer>> DiscoveryService::reachablePeers()
{
    const Timestamp now = clock_.now();

    auto listed = contacts_.listPeersSeenSince(now - kPeerReachableWindow);
    if (listed.isError()) return listed.result();

    std::vector<Peer> peers = listed.value();
    peers.erase(std::remove_if(peers.begin(), peers.end(),
                               [&](const Peer& p) {
                                   return !p.isReachable(now) || contacts_.isBlocked(p.peerId);
                               }),
                peers.end());

    // Strongest first — the UI renders this order directly.
    std::stable_sort(peers.begin(), peers.end(), [](const Peer& a, const Peer& b) {
        return static_cast<int>(a.signal) < static_cast<int>(b.signal);
    });
    return peers;
}

Outcome<PeerHandle> DiscoveryService::handleFor(const PeerId& peerId) const
{
    const auto it = handles_.find(peerId);
    if (it == handles_.end()) {
        return Outcome<PeerHandle>::error(ErrorCode::NotFound,
                                          "That person is no longer in range.");
    }
    return it->second;
}

void DiscoveryService::rememberHandle(const PeerHandle& handle)
{
    handles_[handle.peerId] = handle;
}

void DiscoveryService::sweepStalePeers()
{
    const Timestamp now = clock_.now();

    auto listed = contacts_.listPeersSeenSince(now - kPeerReachableWindow * 2);
    if (listed.isError()) return;

    for (const Peer& peer : listed.value()) {
        if (peer.isReachable(now)) continue;
        if (announced_.erase(peer.peerId) == 0) continue;  // already reported gone

        // Three missed announces: the peer is gone as far as we can tell.
        events_.onPeerLost(peer.peerId);
        handles_.erase(peer.peerId);
    }

    contacts_.prunePeersOlderThan(now - kPeerPruneAfterMillis);
}

}  // namespace anytime
