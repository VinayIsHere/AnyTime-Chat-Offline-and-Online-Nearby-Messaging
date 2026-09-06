// core/services/TransportRegistry.cpp

#include "core/services/TransportRegistry.h"

#include <algorithm>

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
        if (r.isOk()) anyStarted = true;
        else if (firstError.isOk()) firstError = r;
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
    return transport->openLink(handle);
}

const PeerLinks* TransportRegistry::linksFor(const PeerId& peerId) const
{
    const auto it = peerLinks_.find(peerId);
    return it == peerLinks_.end() ? nullptr : &it->second;
}

// ── ITransportListener — record link state, then fan out ─────────────────────

void TransportRegistry::onPeerSeen(TransportId id, const PeerHandle& handle)
{
    if (downstream_) downstream_->onPeerSeen(id, handle);
}

void TransportRegistry::onLinkUp(TransportId id, LinkId link, const PeerId& peerId)
{
    PeerLinks& entry = peerLinks_[peerId];
    entry.peerId = peerId;
    entry.links[id] = link;
    linkOwners_[link] = peerId;

    if (downstream_) downstream_->onLinkUp(id, link, peerId);
}

void TransportRegistry::onLinkDown(TransportId id, LinkId link, LinkCloseReason reason)
{
    const auto owner = linkOwners_.find(link);
    if (owner != linkOwners_.end()) {
        const PeerId peerId = owner->second;
        linkOwners_.erase(owner);

        const auto entry = peerLinks_.find(peerId);
        if (entry != peerLinks_.end()) {
            entry->second.links.erase(id);
            // Keep the peer even with no links: liveness is announce-driven,
            // and dropping it here would contradict that (section 11).
            if (entry->second.links.empty()) peerLinks_.erase(entry);
        }
    }

    if (downstream_) downstream_->onLinkDown(id, link, reason);
}

void TransportRegistry::onFrame(TransportId id, LinkId link, ByteSpan frame)
{
    if (downstream_) downstream_->onFrame(id, link, frame);
}

void TransportRegistry::onTransportStateChanged(TransportId id, bool available)
{
    if (downstream_) downstream_->onTransportStateChanged(id, available);
}

}  // namespace anytime
