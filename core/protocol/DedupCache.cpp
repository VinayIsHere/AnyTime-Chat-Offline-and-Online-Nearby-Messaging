// core/protocol/DedupCache.cpp

#include "core/protocol/DedupCache.h"

namespace anytime::protocol {

DedupCache::DedupCache(std::size_t capacity, Timestamp ttlMillis)
    : capacity_(capacity), ttlMillis_(ttlMillis)
{
}

DedupCache::Key DedupCache::makeKey(const Packet& packet) noexcept
{
    // FNV-1a over the four keying fields. The payload digest matters: two
    // distinct messages sent in the same second by the same peer would
    // otherwise collide and the second would be silently dropped.
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&h](Byte b) {
        h ^= b;
        h *= 1099511628211ULL;
    };

    for (const Byte b : packet.sender.bytes()) mix(b);

    mix(static_cast<Byte>(packet.timestamp & 0xFF));
    mix(static_cast<Byte>((packet.timestamp >> 8) & 0xFF));
    mix(static_cast<Byte>((packet.timestamp >> 16) & 0xFF));
    mix(static_cast<Byte>((packet.timestamp >> 24) & 0xFF));

    mix(static_cast<Byte>(packet.type));

    for (const Byte b : packet.payload) mix(b);

    return h;
}

bool DedupCache::observe(const Packet& packet, Timestamp now)
{
    evictExpired(now);

    const Key key = makeKey(packet);
    const auto found = index_.find(key);

    if (found != index_.end()) {
        // Seen before. Refresh its position so a packet that keeps arriving
        // stays cached rather than aging out and being reprocessed.
        entries_.splice(entries_.begin(), entries_, found->second);
        found->second->insertedAt = now;
        return false;
    }

    entries_.push_front(Entry{key, now});
    index_[key] = entries_.begin();

    while (entries_.size() > capacity_) {
        index_.erase(entries_.back().key);
        entries_.pop_back();
    }
    return true;
}

bool DedupCache::contains(const Packet& packet, Timestamp now) const
{
    const auto found = index_.find(makeKey(packet));
    if (found == index_.end()) return false;
    return (now - found->second->insertedAt) <= ttlMillis_;
}

void DedupCache::evictExpired(Timestamp now)
{
    // Entries are ordered most-recent-first, so everything expired sits at the
    // tail and eviction stops at the first live entry.
    while (!entries_.empty() && (now - entries_.back().insertedAt) > ttlMillis_) {
        index_.erase(entries_.back().key);
        entries_.pop_back();
    }
}

void DedupCache::clear()
{
    entries_.clear();
    index_.clear();
}

}  // namespace anytime::protocol
