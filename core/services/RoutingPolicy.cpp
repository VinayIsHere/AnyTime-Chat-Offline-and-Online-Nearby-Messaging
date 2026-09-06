// core/services/RoutingPolicy.cpp

#include "core/services/RoutingPolicy.h"

#include <algorithm>

#include "core/services/TransportRegistry.h"

namespace anytime {

RoutingPolicy::RoutingPolicy(TransportRegistry& registry) : registry_(registry) {}

std::optional<TransportId> RoutingPolicy::select(const PeerId& peerId, PayloadClass cls) const
{
    const std::vector<TransportId> ranked = candidates(peerId, cls);
    if (ranked.empty()) return std::nullopt;
    return ranked.front();
}

std::vector<TransportId> RoutingPolicy::candidates(const PeerId&, PayloadClass cls) const
{
    struct Scored {
        TransportId id;
        int         score;
    };
    std::vector<Scored> scored;

    for (ITransport* transport : registry_.available()) {
        const TransportCapabilities caps = transport->caps();

        // Control traffic — handshakes, announces, acks — has to travel on a
        // transport that can actually reach strangers and relay.
        if (cls == PayloadClass::Control && !caps.supportsMesh) continue;

        int score = 0;
        switch (cls) {
            case PayloadClass::Bulk:
                // Prefer real throughput; a bulk-capable link is worth a lot.
                score = (caps.supportsBulk ? 1000 : 0)
                        + static_cast<int>(caps.throughputClass) * 100
                        - static_cast<int>(caps.energyCost) * 10;
                break;

            case PayloadClass::Control:
            case PayloadClass::Text:
                // Text is tiny. Cheapest energy wins; throughput is irrelevant,
                // and asking the user for consent to send one line would be absurd.
                score = 1000 - static_cast<int>(caps.energyCost) * 100
                        - (caps.requiresUserConsent ? 500 : 0);
                break;
        }
        scored.push_back({transport->id(), score});
    }

    std::stable_sort(scored.begin(), scored.end(),
                     [](const Scored& a, const Scored& b) { return a.score > b.score; });

    std::vector<TransportId> out;
    out.reserve(scored.size());
    for (const Scored& s : scored) out.push_back(s.id);
    return out;
}

}  // namespace anytime
