// core/protocol/Fragmenter.cpp
//
// Fragment payload layout:
//   [ fragmentId : 8 ][ index : u16 ][ total : u16 ][ slice ]
//
// Every bound here exists because the far side is untrusted: a peer can claim
// any total, any index, and never send the rest. Assemblies are therefore capped
// in count, in bytes and in age.

#include "core/protocol/Fragmenter.h"

#include <cstring>

namespace anytime::protocol {

namespace {

void appendU16(ByteBuffer& out, std::uint16_t v)
{
    out.push_back(static_cast<Byte>((v >> 8) & 0xFF));
    out.push_back(static_cast<Byte>(v & 0xFF));
}

std::uint16_t readU16(ByteSpan s, std::size_t offset)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(s[offset]) << 8) | s[offset + 1]);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

FragmentId Fragmenter::newFragmentId(const PeerId& sender, Timestamp now)
{
    // Unique enough within the 30 s reassembly window: sender bytes mixed with
    // the clock. Collisions across peers are impossible because the reassembly
    // key includes the sender.
    FragmentId id{};
    const PeerIdBytes& bytes = sender.bytes();

    for (std::size_t i = 0; i < kFragmentIdBytes; ++i) {
        id[i] = static_cast<Byte>(bytes[i] ^ static_cast<Byte>((now >> (8 * (i % 8))) & 0xFF));
    }
    return id;
}

Outcome<std::vector<ByteBuffer>> Fragmenter::split(ByteSpan packet, std::size_t maxFrameBytes,
                                                   const FragmentId& fragmentId)
{
    if (maxFrameBytes <= kFragmentHeaderBytes) {
        return Outcome<std::vector<ByteBuffer>>::error(
            ErrorCode::InvalidArgument, "Frame budget is smaller than the fragment header.");
    }

    std::vector<ByteBuffer> frames;

    // Already fits: hand it back untouched rather than wrapping a single
    // fragment, which would waste 12 bytes on every small packet.
    if (packet.size() <= maxFrameBytes) {
        frames.push_back(packet.toBuffer());
        return frames;
    }

    const std::size_t sliceSize = maxFrameBytes - kFragmentHeaderBytes;
    const std::size_t total = (packet.size() + sliceSize - 1) / sliceSize;

    if (total > 0xFFFF) {
        return Outcome<std::vector<ByteBuffer>>::error(ErrorCode::InvalidArgument,
                                                       "Packet needs more than 65535 fragments.");
    }

    frames.reserve(total);
    for (std::size_t index = 0; index < total; ++index) {
        const std::size_t offset = index * sliceSize;
        const std::size_t length = std::min(sliceSize, packet.size() - offset);

        ByteBuffer frame;
        frame.reserve(kFragmentHeaderBytes + length);
        frame.insert(frame.end(), fragmentId.begin(), fragmentId.end());
        appendU16(frame, static_cast<std::uint16_t>(index));
        appendU16(frame, static_cast<std::uint16_t>(total));
        frame.insert(frame.end(), packet.begin() + offset, packet.begin() + offset + length);

        frames.push_back(std::move(frame));
    }
    return frames;
}

// ─────────────────────────────────────────────────────────────────────────────

Reassembler::Reassembler(std::size_t maxConcurrent, Timestamp timeoutMillis, std::size_t maxBytes)
    : maxConcurrent_(maxConcurrent), timeoutMillis_(timeoutMillis), maxBytes_(maxBytes)
{
}

Reassembler::Key Reassembler::makeKey(const PeerId& sender, const FragmentId& fragmentId)
{
    // Sender-scoped, so two peers may use the same fragment id without colliding.
    std::string key = sender.toHex();
    key.reserve(key.size() + kFragmentIdBytes * 2);

    static constexpr char hex[] = "0123456789abcdef";
    for (const Byte b : fragmentId) {
        key.push_back(hex[(b >> 4) & 0x0F]);
        key.push_back(hex[b & 0x0F]);
    }
    return key;
}

Outcome<std::optional<ByteBuffer>> Reassembler::accept(const PeerId& sender,
                                                       ByteSpan fragmentPayload, Timestamp now)
{
    if (fragmentPayload.size() < kFragmentHeaderBytes) {
        return Outcome<std::optional<ByteBuffer>>::error(ErrorCode::MalformedPacket,
                                                         "Fragment shorter than its header.");
    }

    FragmentId fragmentId{};
    std::memcpy(fragmentId.data(), fragmentPayload.data(), kFragmentIdBytes);

    const std::uint16_t index = readU16(fragmentPayload, kFragmentIdBytes);
    const std::uint16_t total = readU16(fragmentPayload, kFragmentIdBytes + 2);

    if (total == 0 || index >= total) {
        return Outcome<std::optional<ByteBuffer>>::error(ErrorCode::MalformedPacket,
                                                         "Fragment index out of range.");
    }

    evictStale(now);

    const Key key = makeKey(sender, fragmentId);
    auto it = assemblies_.find(key);

    if (it == assemblies_.end()) {
        // Refuse to open a new assembly once the cap is reached, rather than
        // evicting a live one — otherwise a flood of one-fragment assemblies
        // would starve every genuine transfer.
        if (assemblies_.size() >= maxConcurrent_) {
            return Outcome<std::optional<ByteBuffer>>::error(
                ErrorCode::Internal, "Too many reassemblies in flight.");
        }

        Assembly fresh;
        fresh.total = total;
        fresh.startedAt = now;
        fresh.slices.resize(total);
        fresh.present.assign(total, false);
        it = assemblies_.emplace(key, std::move(fresh)).first;
    }

    Assembly& assembly = it->second;

    // A peer that changes `total` mid-transfer is malformed, not merely late.
    if (assembly.total != total) {
        assemblies_.erase(it);
        return Outcome<std::optional<ByteBuffer>>::error(ErrorCode::MalformedPacket,
                                                         "Fragment count changed mid-assembly.");
    }
    if (assembly.present[index]) return std::optional<ByteBuffer>{};  // duplicate, ignore

    const ByteSpan slice =
        fragmentPayload.subspan(kFragmentHeaderBytes, fragmentPayload.size() - kFragmentHeaderBytes);

    if (assembly.receivedBytes + slice.size() > maxBytes_) {
        assemblies_.erase(it);
        return Outcome<std::optional<ByteBuffer>>::error(ErrorCode::MalformedPacket,
                                                         "Reassembly exceeds the size cap.");
    }

    assembly.slices[index] = slice.toBuffer();
    assembly.present[index] = true;
    assembly.receivedBytes += slice.size();

    for (const bool have : assembly.present) {
        if (!have) return std::optional<ByteBuffer>{};  // still incomplete
    }

    ByteBuffer complete;
    complete.reserve(assembly.receivedBytes);
    for (const ByteBuffer& slice_ : assembly.slices) {
        complete.insert(complete.end(), slice_.begin(), slice_.end());
    }
    assemblies_.erase(it);

    return std::optional<ByteBuffer>{std::move(complete)};
}

void Reassembler::evictStale(Timestamp now)
{
    for (auto it = assemblies_.begin(); it != assemblies_.end();) {
        it = (now - it->second.startedAt) > timeoutMillis_ ? assemblies_.erase(it) : std::next(it);
    }
}

}  // namespace anytime::protocol
