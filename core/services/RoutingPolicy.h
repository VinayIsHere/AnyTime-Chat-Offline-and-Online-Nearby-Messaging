// core/services/RoutingPolicy.h
//
// Chooses a transport for a (peer, payload class) pair (§06).
//
// In v1 this is trivial — only BLE and loopback exist, so everything goes over
// whichever is available. WRITE IT AS A REAL POLICY OBJECT ANYWAY. The entire
// point is that phase 5 edits this one class instead of threading a new concept
// through every service.

#pragma once

#include <optional>
#include <vector>

#include "core/domain/Types.h"
#include "core/ports/ITransport.h"

namespace anytime {

class TransportRegistry;

class RoutingPolicy {
public:
    explicit RoutingPolicy(TransportRegistry&);

    /// Picks the best available transport for this payload. Returns nullopt when
    /// nothing is available — the caller then queues to the outbox.
    ///
    /// Selection rules, in order:
    ///   Control → must be a mesh transport (supportsMesh), lowest energy first
    ///   Text    → any available transport, lowest energy first
    ///   Bulk    → prefer supportsBulk and highest throughputClass;
    ///             fall back to Text behaviour when no bulk transport exists
    std::optional<TransportId> select(const PeerId&, PayloadClass) const;

    /// Every AVAILABLE transport, ranked best-first for this payload class.
    ///
    /// Ranks on capability alone — it deliberately does NOT know which
    /// transports have an open link to this peer, because links are the
    /// registry's business, not the policy's. So the top entry may well be
    /// unable to carry anything right now.
    ///
    /// MeshEngine::sendDirected consumes this list in order, skipping entries
    /// with no open link. That is the fallback path: a photo ranks Nearby first
    /// but travels over BLE when no Nearby link is up.
    ///
    /// (An earlier comment here claimed this returned only transports with a
    /// live link, and that DeliveryService consumed it. Neither was true —
    /// DeliveryService has no RoutingPolicy at all.)
    std::vector<TransportId> candidates(const PeerId&, PayloadClass) const;

private:
    TransportRegistry& registry_;
};

}  // namespace anytime
