// core/services/DiscoveryService.cpp

#include "core/services/DiscoveryService.h"

#include <algorithm>

#include "core/log/Log.h"
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

void DiscoveryService::setLocalPeerId(const PeerId& peerId)
{
    localPeerId_ = peerId;
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
    // Remembered even if starting fails below, so resume() can finish the job
    // once a radio appears.
    desiredMode_ = mode;

    if (running_ && mode == mode_) return Result::ok();

    const Result r = registry_.startDiscovery(mode);
    if (r.isError()) {
        AT_LOG_WARN("discovery", "cannot scan yet: {}", r.message());
        events_.onRadioStateChanged(false, "no_transport");
        return r;
    }

    running_ = true;
    mode_ = mode;

    if (sweepTimer_ == kInvalidTimer) {
        sweepTimer_ = clock_.scheduleRepeating(kPeerSweepIntervalMillis,
                                               [this] { sweepStalePeers(); });
    }

    AT_LOG_INFO("discovery", "scanning for people nearby");
    events_.onRadioStateChanged(true, "");
    return Result::ok();
}

void DiscoveryService::stop()
{
    desiredMode_ = DiscoveryMode::Off;
    if (!running_) return;

    registry_.stopDiscovery();
    running_ = false;
    mode_ = DiscoveryMode::Off;

    if (sweepTimer_ != kInvalidTimer) {
        clock_.cancel(sweepTimer_);
        sweepTimer_ = kInvalidTimer;
    }
    AT_LOG_INFO("discovery", "scanning stopped");
}

void DiscoveryService::resume()
{
    if (desiredMode_ == DiscoveryMode::Off) return;

    if (running_) {
        // Already scanning on some transport. Starting again is idempotent for
        // those, and picks up any transport that came up after the scan began.
        registry_.startDiscovery(desiredMode_);
        return;
    }

    AT_LOG_DEBUG("discovery", "a transport came up; resuming the requested scan");
    start(desiredMode_);
}

void DiscoveryService::onAnnounce(const PeerId& peerId, const std::string& nickname,
                                  const PublicKey& staticKey, int rssi)
{
    const Timestamp now = clock_.now();

    // A blocked peer is dropped before it can reach the UI at all. Blocking is
    // keyed on the fingerprint, so renaming does not evade it (section 08).
    if (contacts_.isBlocked(peerId)) {
        AT_LOG_TRACE("discovery", "announce from blocked peer {} ignored", peerId.toHex());
        return;
    }

    const bool isNew = announced_.find(peerId) == announced_.end();

    Peer peer;
    peer.peerId = peerId;
    peer.staticPublicKey = staticKey;
    peer.nickname = nickname;
    peer.signal = bandForRssi(rssi);
    peer.verifiedAnnounceAt = now;
    peer.lastSeenAt = now;

    if (const Result stored = contacts_.upsertPeer(peer); stored.isError()) {
        AT_LOG_ERROR("discovery", "could not record peer {}: {}", peerId.toHex(),
                     stored.message());
        return;
    }

    // Only announce arrival once. Subsequent announces refresh the window
    // silently, otherwise the Nearby list would churn every few seconds.
    if (isNew) {
        announced_.insert(peerId);
        AT_LOG_INFO("discovery", "peer {} in range ({} dBm)", peerId.toHex(), rssi);
        events_.onPeerDiscovered(peer);
    }
}

Outcome<std::vector<Peer>> DiscoveryService::reachablePeers()
{
    const Timestamp now = clock_.now();

    auto listed = contacts_.listPeersSeenSince(now - kPeerReachableWindow);
    if (listed.isError()) {
        AT_LOG_ERROR("discovery", "could not list peers: {}", listed.result().message());
        return listed.result();
    }

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

void DiscoveryService::onPeerSeen(TransportId transport, const PeerHandle& handle)
{
    if (handle.peerId.isNull()) return;

    handles_[handle.peerId] = handle;
    maybeOpenLink(transport, handle);
}

void DiscoveryService::maybeOpenLink(TransportId transport, const PeerHandle& handle)
{
    const PeerId& peer = handle.peerId;

    if (localPeerId_.isNull() || peer == localPeerId_) return;
    if (contacts_.isBlocked(peer)) return;

    const PeerLinks* links = registry_.linksFor(peer);
    if (links != nullptr && links->hasLink(transport)) {
        firstSeenUnlinked_.erase(peer);
        return;
    }

    const Timestamp now = clock_.now();
    const Timestamp firstSeen = firstSeenUnlinked_.emplace(peer, now).first->second;

    // TIE-BREAK. Both devices see each other, and two links per pair would
    // double the radio time — and on BLE the stack may refuse the second. So the
    // lower peer id connects and the higher one waits. The wait is bounded:
    // radio visibility is not always symmetric, and a lower peer that cannot see
    // us must not leave the pair unconnected forever.
    const bool weInitiate = localPeerId_ < peer;
    const bool graceExpired = now - firstSeen >= kLinkInitiationGraceMillis;
    if (!weInitiate && !graceExpired) return;

    const auto lastAttempt = lastLinkAttempt_.find(peer);
    if (lastAttempt != lastLinkAttempt_.end() && now - lastAttempt->second < kLinkRetryMillis) {
        return;
    }
    lastLinkAttempt_[peer] = now;

    AT_LOG_DEBUG("discovery", "opening {} link to {}{}", toString(transport), peer.toHex(),
                 weInitiate ? "" : " (it did not connect to us within the grace period)");

    const auto link = registry_.ensureLink(transport, handle);
    if (link.isError()) {
        AT_LOG_DEBUG("discovery", "could not open {} link to {}: {}", toString(transport),
                     peer.toHex(), link.result().message());
    }
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
        AT_LOG_INFO("discovery", "peer {} out of range", peer.peerId.toHex());
        events_.onPeerLost(peer.peerId);
        handles_.erase(peer.peerId);
        firstSeenUnlinked_.erase(peer.peerId);
        lastLinkAttempt_.erase(peer.peerId);
    }

    contacts_.prunePeersOlderThan(now - kPeerPruneAfterMillis);
}

}  // namespace anytime
