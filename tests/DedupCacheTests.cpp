// tests/DedupCacheTests.cpp
//
// Without duplicate suppression a relayed packet loops between neighbours until
// its TTL burns out. These tests pin the two properties that matter: a repeat is
// recognised, and two genuinely different packets are never confused for one
// another — including when they share a sender and a timestamp, which is exactly
// what happens when someone sends two messages in the same second.

#include "tests/TestHarness.h"

#include "core/protocol/DedupCache.h"

namespace anytime::test {

using namespace anytime::protocol;

namespace {

PeerId peerFrom(Byte fill)
{
    PeerIdBytes bytes{};
    bytes.fill(fill);
    return PeerId{bytes};
}

Packet makePacket(Byte sender, std::uint32_t timestamp, const ByteBuffer& payload,
                  PacketType type = PacketType::NoiseMessage)
{
    Packet packet;
    packet.type = type;
    packet.timestamp = timestamp;
    packet.sender = peerFrom(sender);
    packet.payload = payload;
    return packet;
}

}  // namespace

void runDedupCacheTests(Registry& r)
{
    r.section("DedupCache — duplicate suppression");

    {
        DedupCache cache;
        const ByteBuffer payload{1, 2, 3};
        const Packet packet = makePacket(0xA1, 1000, payload);

        r.check(cache.observe(packet, 0), "first sighting is accepted");
        r.check(!cache.observe(packet, 0), "second sighting is rejected");
        r.check(!cache.observe(packet, 100), "still rejected slightly later");
    }

    {
        DedupCache cache;
        const ByteBuffer payloadA{1, 2, 3};
        const ByteBuffer payloadB{1, 2, 4};

        // Same sender, same second, different content. Keying without the
        // payload digest would silently drop the second message.
        r.check(cache.observe(makePacket(0xA1, 1000, payloadA), 0),
                "accepts the first of two same-second messages");
        r.check(cache.observe(makePacket(0xA1, 1000, payloadB), 0),
                "accepts the second — payload digest keeps them distinct");
    }

    {
        DedupCache cache;
        const ByteBuffer payload{5};
        r.check(cache.observe(makePacket(0xA1, 1000, payload), 0), "accepts from sender A");
        r.check(cache.observe(makePacket(0xB2, 1000, payload), 0),
                "accepts the identical payload from a different sender");
    }

    {
        DedupCache cache;
        const ByteBuffer payload{5};
        r.check(cache.observe(makePacket(0xA1, 1000, payload, PacketType::NoiseMessage), 0),
                "accepts a NoiseMessage");
        r.check(cache.observe(makePacket(0xA1, 1000, payload, PacketType::Ack), 0),
                "accepts an Ack with otherwise identical fields");
    }

    r.section("DedupCache — expiry and capacity");

    {
        DedupCache cache(1000, 5000);  // 5 s TTL
        const ByteBuffer payload{7};
        const Packet packet = makePacket(0xC3, 2000, payload);

        cache.observe(packet, 0);
        r.check(!cache.observe(packet, 4000), "still a duplicate inside the TTL");

        // A packet seen long ago has aged out; seeing it again is legitimate,
        // because the sender must have genuinely re-sent it.
        DedupCache fresh(1000, 5000);
        fresh.observe(packet, 0);
        r.check(fresh.observe(packet, 60000), "accepted again once the TTL has passed");
    }

    {
        DedupCache cache(4, 1'000'000);  // capacity 4, effectively no expiry

        for (int i = 0; i < 6; ++i) {
            const ByteBuffer payload{static_cast<Byte>(i)};
            cache.observe(makePacket(0xD4, 3000, payload), 0);
        }
        r.check(cache.size() <= 4, "never grows past its capacity");

        // The two oldest were evicted, so they read as new again — the accepted
        // cost of a bounded cache.
        const ByteBuffer oldest{0};
        r.check(cache.observe(makePacket(0xD4, 3000, oldest), 0),
                "an evicted entry is seen as new again");
    }

    {
        DedupCache cache;
        const ByteBuffer payload{9};
        cache.observe(makePacket(0xE5, 4000, payload), 0);
        cache.clear();
        r.check(cache.size() == 0, "clear empties the cache");
        r.check(cache.observe(makePacket(0xE5, 4000, payload), 0), "accepted again after clear");
    }
}

}  // namespace anytime::test
