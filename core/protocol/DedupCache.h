// core/protocol/DedupCache.h
//
// Duplicate suppression for controlled flooding (§07). Without this a relayed
// packet loops between neighbours until its TTL burns out, multiplying traffic
// and battery cost.
//
// Keyed on (sender, timestamp, type, payload digest) — bitchat's scheme. The
// digest matters: two distinct messages sent in the same second by the same peer
// must not collide.

#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

#include "core/domain/Types.h"
#include "core/protocol/Packet.h"

namespace anytime::protocol {

class DedupCache {
public:
    explicit DedupCache(std::size_t capacity  = kDedupCapacity,
                        Timestamp   tttMillis = kDedupTtlMillis);

    /// Records the packet and reports whether it was already known.
    /// Returns true when this is the FIRST sighting (i.e. the caller should
    /// process and possibly relay it); false when it is a duplicate to drop.
    bool observe(const Packet&, Timestamp now);

    /// Test-only query that does not mutate LRU order.
    bool contains(const Packet&, Timestamp now) const;

    void      evictExpired(Timestamp now);
    void      clear();
    std::size_t size() const noexcept { return entries_.size(); }

private:
    using Key = std::uint64_t;  ///< 64-bit digest of the four keying fields

    static Key makeKey(const Packet&) noexcept;

    struct Entry {
        Key       key;
        Timestamp insertedAt;
    };

    std::size_t capacity_;
    Timestamp   ttlMillis_;

    /// Front is most-recently used. The map indexes into the list so both
    /// lookup and eviction are O(1).
    std::list<Entry>                                          entries_;
    std::unordered_map<Key, std::list<Entry>::iterator>       index_;
};

}  // namespace anytime::protocol
