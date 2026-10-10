// tests/RoutingPolicyTests.cpp
//
// Transport selection — the class that decides whether a payload goes over BLE
// or a bulk transport (§06).
//
// WHY THIS SUITE EXISTS. RoutingPolicy looked correct for a long time while
// being unreachable: DeliveryService hard-coded PayloadClass::Text and
// MeshEngine discarded the argument, so `Bulk` never once reached this class and
// a photo could not have selected a bulk transport even if one had been
// registered. Nothing failed — the wrong answer was simply never observed.
//
// These tests pin the behaviour with FAKE transports, so the selection rules are
// provable on a desktop with no radio and before the Nearby transport exists.

#include "tests/TestHarness.h"

#include <memory>

#include "core/services/RoutingPolicy.h"
#include "core/services/TransportRegistry.h"

namespace anytime::test {

namespace {

/// A transport that does nothing but report capabilities. RoutingPolicy reads
/// only caps() and isAvailable(), so nothing else needs to be real.
class FakeTransport final : public ITransport {
public:
    FakeTransport(TransportId id, TransportCapabilities caps, bool available)
        : id_(id), caps_(caps), available_(available)
    {
    }

    TransportId           id()   const override { return id_; }
    TransportCapabilities caps() const override { return caps_; }
    bool                  isAvailable() const override { return available_; }

    Result start(const PeerId&) override { return Result::ok(); }
    void   stop() override {}
    Result startDiscovery(DiscoveryMode) override { return Result::ok(); }
    void   stopDiscovery() override {}
    Result startAdvertising() override { return Result::ok(); }
    void   stopAdvertising() override {}

    Outcome<LinkId> openLink(const PeerHandle&) override { return LinkId{1}; }
    void            closeLink(LinkId) override {}
    Result          send(LinkId, ByteSpan) override { return Result::ok(); }
    void            setListener(ITransportListener*) override {}

private:
    TransportId           id_;
    TransportCapabilities caps_;
    bool                  available_;
};

/// BLE: cheap, mesh-capable, low throughput, no consent prompt.
TransportCapabilities bleCaps()
{
    TransportCapabilities caps;
    caps.maxFramePayload = 480;
    caps.throughputClass = 0;
    caps.supportsBulk = false;
    caps.supportsMesh = true;
    caps.requiresUserConsent = false;
    caps.energyCost = EnergyCost::Low;
    return caps;
}

/// Nearby Connections: fast and bulk-capable, but expensive, point-to-point
/// rather than mesh, and it shows a system consent dialog per connection.
TransportCapabilities nearbyCaps()
{
    TransportCapabilities caps;
    caps.maxFramePayload = 32 * 1024;
    caps.throughputClass = 2;
    caps.supportsBulk = true;
    caps.supportsMesh = false;
    caps.requiresUserConsent = true;
    caps.energyCost = EnergyCost::High;
    return caps;
}

void addFake(TransportRegistry& registry, TransportId id, TransportCapabilities caps,
             bool available = true)
{
    registry.addTransport(std::make_unique<FakeTransport>(id, caps, available));
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

void runRoutingPolicyTests(Registry& r)
{
    r.section("RoutingPolicy");

    const PeerId peer;  // selection does not depend on the peer today

    // ── BLE only: what every device has ─────────────────────────────────────

    {
        TransportRegistry registry;
        addFake(registry, TransportId::BleGatt, bleCaps());
        RoutingPolicy policy(registry);

        const auto control = policy.select(peer, PayloadClass::Control);
        r.check(control.has_value() && *control == TransportId::BleGatt,
                "control traffic goes over BLE");

        const auto text = policy.select(peer, PayloadClass::Text);
        r.check(text.has_value() && *text == TransportId::BleGatt, "text goes over BLE");

        // The documented fallback: Bulk behaves like Text when nothing can do
        // better. A photo must still send on a phone with no Play Services.
        const auto bulk = policy.select(peer, PayloadClass::Bulk);
        r.check(bulk.has_value() && *bulk == TransportId::BleGatt,
                "bulk falls back to BLE when no bulk transport is registered");
    }

    // ── BLE + Nearby: the shipping Android configuration ────────────────────

    {
        TransportRegistry registry;
        addFake(registry, TransportId::BleGatt, bleCaps());
        addFake(registry, TransportId::NearbyConnections, nearbyCaps());
        RoutingPolicy policy(registry);

        // THE ASSERTION THIS WHOLE SUITE EXISTS FOR.
        const auto bulk = policy.select(peer, PayloadClass::Bulk);
        r.check(bulk.has_value() && *bulk == TransportId::NearbyConnections,
                "a photo selects the bulk transport");

        // A handshake must never ride a transport that cannot reach strangers
        // and needs a consent dialog before it carries anything.
        const auto control = policy.select(peer, PayloadClass::Control);
        r.check(control.has_value() && *control == TransportId::BleGatt,
                "control refuses a non-mesh transport even when it is faster");

        // Asking the user to approve a connection to send one line would be
        // absurd, and the energy cost is not worth it either.
        const auto text = policy.select(peer, PayloadClass::Text);
        r.check(text.has_value() && *text == TransportId::BleGatt,
                "text stays on the cheap radio rather than the fast one");

        // Ordering matters for DeliveryService's fallback: best first.
        const auto ranked = policy.candidates(peer, PayloadClass::Bulk);
        r.check(ranked.size() == 2, "both transports are bulk candidates");
        r.check(!ranked.empty() && ranked.front() == TransportId::NearbyConnections,
                "the bulk transport ranks first for a photo");
        r.check(ranked.size() == 2 && ranked.back() == TransportId::BleGatt,
                "BLE remains the fallback behind it");

        const auto controlRanked = policy.candidates(peer, PayloadClass::Control);
        r.check(controlRanked.size() == 1,
                "a non-mesh transport is not even a control candidate");
    }

    // ── Play Services missing: Nearby registered but unusable ───────────────
    //
    // The de-Googled case. Photos must degrade to slow rather than to broken.

    {
        TransportRegistry registry;
        addFake(registry, TransportId::BleGatt, bleCaps());
        addFake(registry, TransportId::NearbyConnections, nearbyCaps(), /*available=*/false);
        RoutingPolicy policy(registry);

        const auto bulk = policy.select(peer, PayloadClass::Bulk);
        r.check(bulk.has_value() && *bulk == TransportId::BleGatt,
                "an unavailable bulk transport is skipped and the photo still sends");

        const auto ranked = policy.candidates(peer, PayloadClass::Bulk);
        r.check(ranked.size() == 1, "an unavailable transport is not a candidate at all");
    }

    // ── Loopback, the desktop configuration ─────────────────────────────────
    //
    // Loopback advertises mesh support, so the desktop build must still be able
    // to route every class — otherwise two windows could not talk.

    {
        TransportRegistry registry;
        TransportCapabilities loopback = bleCaps();
        loopback.maxFramePayload = 480;
        addFake(registry, TransportId::Loopback, loopback);
        RoutingPolicy policy(registry);

        r.check(policy.select(peer, PayloadClass::Control).has_value(),
                "loopback carries control on the desktop");
        r.check(policy.select(peer, PayloadClass::Bulk).has_value(),
                "loopback carries bulk on the desktop");
    }

    // ── Link awareness: the ingredients the fallback depends on ─────────────
    //
    // RoutingPolicy ranks on capability and knows nothing about links, so the
    // top-ranked transport is routinely unable to carry anything. These pin the
    // registry side of what MeshEngine::sendDirected walks: rank first, then
    // skip the ones with no open link.

    {
        TransportRegistry registry;
        addFake(registry, TransportId::BleGatt, bleCaps());
        addFake(registry, TransportId::NearbyConnections, nearbyCaps());
        RoutingPolicy policy(registry);

        PeerIdBytes bytes{};
        bytes[0] = 0x11;
        const PeerId linked{bytes};

        const auto ranked = policy.candidates(linked, PayloadClass::Bulk);
        r.check(ranked.size() == 2 && ranked.front() == TransportId::NearbyConnections,
                "bulk ranks Nearby first on capability alone");
        r.check(registry.linksFor(linked) == nullptr,
                "ranking says nothing about whether a link exists");

        // A BLE link comes up while Nearby still has none — exactly the state a
        // photo meets, since Nearby links are opened on demand.
        registry.onLinkUp(TransportId::BleGatt, 7, linked);

        const PeerLinks* links = registry.linksFor(linked);
        r.check(links != nullptr, "the peer now has a link");
        r.check(links && links->hasLink(TransportId::BleGatt), "the BLE link is tracked");
        r.check(links && !links->hasLink(TransportId::NearbyConnections),
                "the top-ranked transport has no link, so sending must fall back");
        r.check(links && links->linkFor(TransportId::NearbyConnections) == kInvalidLink,
                "a transport with no link reports kInvalidLink, which is the skip signal");
        r.check(links && links->linkFor(TransportId::BleGatt) == 7,
                "the fallback transport reports the link to send on");
    }

    // ── Nothing available ───────────────────────────────────────────────────

    {
        TransportRegistry registry;
        RoutingPolicy policy(registry);

        r.check(!policy.select(peer, PayloadClass::Text).has_value(),
                "no transports means no route, so the caller queues to the outbox");
        r.check(policy.candidates(peer, PayloadClass::Bulk).empty(),
                "no transports means no candidates");
    }

    // Radio off with a bulk transport still up: nothing should crash, and the
    // photo should take the only road available.
    {
        TransportRegistry registry;
        addFake(registry, TransportId::BleGatt, bleCaps(), /*available=*/false);
        addFake(registry, TransportId::NearbyConnections, nearbyCaps());
        RoutingPolicy policy(registry);

        const auto bulk = policy.select(peer, PayloadClass::Bulk);
        r.check(bulk.has_value() && *bulk == TransportId::NearbyConnections,
                "with BLE down a photo still routes over the bulk transport");

        r.check(!policy.select(peer, PayloadClass::Control).has_value(),
                "with BLE down there is no mesh transport for a handshake");
    }
}

}  // namespace anytime::test
