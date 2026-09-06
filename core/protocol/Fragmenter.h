// core/protocol/Fragmenter.h
//
// Splits oversized packets into transport-sized frames and reassembles them.
//
// THIS LIVES IN THE CORE ON PURPOSE (§11). MTU belongs to the radio, which makes
// it tempting to fragment down in Java — resist that. Fragmentation in the
// platform layer means writing it again in Swift and debugging two reassembly
// implementations that disagree. The transport advertises its frame size through
// TransportCapabilities::maxFramePayload; the core does the splitting.

#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/domain/Result.h"
#include "core/domain/Types.h"
#include "core/protocol/Packet.h"

namespace anytime::protocol {

using FragmentId = std::array<Byte, kFragmentIdBytes>;

class Fragmenter {
public:
    /// Splits `packet` into frames of at most `maxFrameBytes`. Returns a single
    /// frame unchanged when it already fits.
    static Outcome<std::vector<ByteBuffer>> split(ByteSpan packet,
                                                  std::size_t maxFrameBytes,
                                                  const FragmentId&);

    /// Generates a fragment id unique enough within the reassembly window.
    static FragmentId newFragmentId(const PeerId& sender, Timestamp now);
};

/// Accumulates inbound fragments per peer. Bounded on both count and size so a
/// hostile peer cannot exhaust memory by opening assemblies it never completes.
class Reassembler {
public:
    explicit Reassembler(std::size_t maxConcurrent = kMaxConcurrentReassemblies,
                         Timestamp   timeoutMillis = kReassemblyTimeoutMillis,
                         std::size_t maxBytes      = kMaxReassembledBytes);

    /// Feeds one FRAGMENT payload. Returns the completed packet when the last
    /// missing piece arrives, nullopt while still incomplete, or an error for
    /// malformed or over-large input.
    Outcome<std::optional<ByteBuffer>> accept(const PeerId& sender,
                                              ByteSpan fragmentPayload,
                                              Timestamp now);

    /// Drops assemblies past the timeout. Call from the announce timer.
    void evictStale(Timestamp now);

    std::size_t pendingCount() const noexcept { return assemblies_.size(); }

private:
    struct Assembly {
        std::uint16_t              total = 0;
        std::size_t                receivedBytes = 0;
        Timestamp                  startedAt = kNoTimestamp;
        std::vector<ByteBuffer>    slices;      ///< indexed by fragment index
        std::vector<bool>          present;
    };

    /// Keyed on sender + fragment id, so two peers may use the same id safely.
    using Key = std::string;
    static Key makeKey(const PeerId&, const FragmentId&);

    std::size_t maxConcurrent_;
    Timestamp   timeoutMillis_;
    std::size_t maxBytes_;

    std::unordered_map<Key, Assembly> assemblies_;
};

}  // namespace anytime::protocol
