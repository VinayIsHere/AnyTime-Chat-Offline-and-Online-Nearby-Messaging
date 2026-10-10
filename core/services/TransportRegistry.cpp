// core/services/TransportRegistry.cpp

#include "core/services/TransportRegistry.h"

#include <algorithm>

#include "core/log/Log.h"

namespace anytime {

LinkId PeerLinks::linkFor(TransportId t) const
{
    const auto it = links.find(t);
    return it == links.end() ? kInvalidLink : it->second;
}

// ─────────────────────────────────────────────────────────────────────────────

TransportRegistry::TransportRegistry() = default;

TransportRegistry::~TransportRegistry()
{
    // Detach before the transports die, so a late callback cannot reach a
    // half-destroyed registry.
    for (auto& transport : transports_) transport->setListener(nullptr);
}

void TransportRegistry::addTransport(std::unique_ptr<ITransport> transport)
{
    AT_LOG_DEBUG("transport", "registered {}", toString(transport->id()));
    transport->setListener(this);
    transports_.push_back(std::move(transport));
}

void TransportRegistry::setListener(ITransportListener* listener)
{
    downstream_ = listener;
}

Result TransportRegistry::startAll(const PeerId& localPeerId)
{
    Result firstError;
    bool anyStarted = false;

    for (auto& transport : transports_) {
        const Result r = transport->start(localPeerId);
        if (r.isOk()) {
            anyStarted = true;
        } else {
            AT_LOG_DEBUG("transport", "{} not started: {} ({})", toString(transport->id()),
                         r.message(), toString(r.code()));
            if (firstError.isOk()) firstError = r;
        }
    }

    // One working transport is enough to run; a phone with Bluetooth off should
    // still open the app.
    if (anyStarted) return Result::ok();
    return firstError.isError()
               ? firstError
               : Result::error(ErrorCode::TransportUnavailable, "No transports registered.");
}

void TransportRegistry::stopAll()
{
    for (auto& transport : transports_) transport->stop();
    peerLinks_.clear();
    linkOwners_.clear();
    linkTransports_.clear();
    unboundLinks_.clear();
}

bool TransportRegistry::allAvailable() const
{
    if (transports_.empty()) return false;
    return std::all_of(transports_.begin(), transports_.end(),
                       [](const auto& transport) { return transport->isAvailable(); });
}

Result TransportRegistry::startDiscovery(DiscoveryMode mode)
{
    bool any = false;
    for (auto& transport : transports_) {
        if (!transport->isAvailable()) continue;
        if (transport->startDiscovery(mode).isOk()) any = true;
    }
    return any ? Result::ok()
               : Result::error(ErrorCode::TransportUnavailable,
                               "No transport is available to scan with.");
}

void TransportRegistry::stopDiscovery()
{
    for (auto& transport : transports_) transport->stopDiscovery();
}

Result TransportRegistry::setAdvertising(bool enabled)
{
    bool any = false;
    for (auto& transport : transports_) {
        if (!transport->isAvailable()) continue;
        if (enabled) {
            if (transport->startAdvertising().isOk()) any = true;
        } else {
            transport->stopAdvertising();
            any = true;
        }
    }
    return any ? Result::ok()
               : Result::error(ErrorCode::TransportUnavailable, "No transport available.");
}

ITransport* TransportRegistry::find(TransportId id) const
{
    for (const auto& transport : transports_) {
        if (transport->id() == id) return transport.get();
    }
    return nullptr;
}

std::vector<ITransport*> TransportRegistry::available() const
{
    std::vector<ITransport*> out;
    for (const auto& transport : transports_) {
        if (transport->isAvailable()) out.push_back(transport.get());
    }
    return out;
}

Outcome<LinkId> TransportRegistry::ensureLink(TransportId id, const PeerHandle& handle)
{
    if (const PeerLinks* links = linksFor(handle.peerId)) {
        const LinkId existing = links->linkFor(id);
        if (existing != kInvalidLink) return existing;
    }

    ITransport* transport = find(id);
    if (!transport) {
        return Outcome<LinkId>::error(ErrorCode::TransportUnavailable, "Unknown transport.");
    }
    if (!transport->isAvailable()) {
        return Outcome<LinkId>::error(ErrorCode::TransportUnavailable, "Transport is down.");
    }
    return transport->openLink(handle);
}

const PeerLinks* TransportRegistry::linksFor(const PeerId& peerId) const
{
    const auto it = peerLinks_.find(peerId);
    return it == peerLinks_.end() ? nullptr : &it->second;
}

void TransportRegistry::attach(TransportId transport, LinkId link, const PeerId& peerId)
{
    PeerLinks& entry = peerLinks_[peerId];
    entry.peerId = peerId;
    entry.links[transport] = link;  // the newest link becomes the preferred one
    linkOwners_[link] = peerId;
    linkTransports_[link] = transport;
}

bool TransportRegistry::bindLink(TransportId transport, LinkId link, const PeerId& peerId)
{
    if (peerId.isNull()) return false;

    const auto unbound = unboundLinks_.find(link);
    if (unbound == unboundLinks_.end() || unbound->second != transport) return false;

    unboundLinks_.erase(unbound);
    attach(transport, link, peerId);

    AT_LOG_DEBUG("transport", "incoming {} link {} identified as {}", toString(transport), link,
                 peerId.toHex());
    return true;
}

// ── ITransportListener ───────────────────────────────────────────────────────

void TransportRegistry::onPeerSeen(TransportId id, const PeerHandle& handle)
{
    if (downstream_) downstream_->onPeerSeen(id, handle);
}

void TransportRegistry::onLinkUp(TransportId id, LinkId link, const PeerId& peerId)
{
    if (peerId.isNull()) {
        // Incoming, identity unknown until the peer announces. See bindLink().
        unboundLinks_[link] = id;
        AT_LOG_DEBUG("transport", "{} link {} up, peer not yet identified", toString(id), link);
    } else {
        attach(id, link, peerId);
        AT_LOG_DEBUG("transport", "{} link {} up to {}", toString(id), link, peerId.toHex());
    }

    if (downstream_) downstream_->onLinkUp(id, link, peerId);
}

void TransportRegistry::onLinkDown(TransportId id, LinkId link, LinkCloseReason reason)
{
    unboundLinks_.erase(link);

    const auto owner = linkOwners_.find(link);
    if (owner != linkOwners_.end()) {
        const PeerId peerId = owner->second;
        linkOwners_.erase(owner);
        linkTransports_.erase(link);

        const auto entry = peerLinks_.find(peerId);
        if (entry != peerLinks_.end()) {
            auto& links = entry->second.links;
            const auto preferred = links.find(id);

            // Only touch routing if the link that dropped is the one routing was
            // using. A peer can hold two links on one transport — one outgoing,
            // one incoming — and losing the spare must not strand the live one.
            if (preferred != links.end() && preferred->second == link) {
                links.erase(preferred);

                for (const auto& [otherLink, otherOwner] : linkOwners_) {
                    const auto transport = linkTransports_.find(otherLink);
                    if (otherOwner == peerId && transport != linkTransports_.end()
                        && transport->second == id) {
                        links[id] = otherLink;
                        break;
                    }
                }
            }

            // Liveness is announce-driven, so dropping the routing entry here
            // does not make the peer "lost" (§11).
            if (links.empty()) peerLinks_.erase(entry);
        }
    }

    AT_LOG_DEBUG("transport", "{} link {} down (reason {})", toString(id), link,
                 static_cast<int>(reason));

    if (downstream_) downstream_->onLinkDown(id, link, reason);
}

void TransportRegistry::onFrame(TransportId id, LinkId link, ByteSpan frame)
{
    if (downstream_) downstream_->onFrame(id, link, frame);
}

void TransportRegistry::onTransportStateChanged(TransportId id, bool available)
{
    AT_LOG_INFO("transport", "{} is now {}", toString(id), available ? "available" : "unavailable");
    if (downstream_) downstream_->onTransportStateChanged(id, available);
}

}  // namespace anytime
