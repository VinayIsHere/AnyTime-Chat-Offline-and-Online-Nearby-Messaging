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

    /// Every transport that currently has a link to this peer, best first.
    /// Used by DeliveryService when a send fails and a fallback is worth trying.
    std::vector<TransportId> candidates(const PeerId&, PayloadClass) const;

private:
    TransportRegistry& registry_;
};

}  // namespace anytime
