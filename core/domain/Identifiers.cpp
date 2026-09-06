// core/domain/Identifiers.cpp

#include "core/domain/Identifiers.h"

#include <algorithm>
#include <cstring>

namespace anytime {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

PeerId PeerId::fromFingerprint(ByteSpan fingerprint)
{
    PeerIdBytes bytes{};
    const std::size_t n = std::min(fingerprint.size(), kPeerIdBytes);
    if (n > 0) std::memcpy(bytes.data(), fingerprint.data(), n);
    return PeerId{bytes};
}

std::optional<PeerId> PeerId::fromHex(const std::string& hex)
{
    if (hex.size() != kPeerIdBytes * 2) return std::nullopt;

    PeerIdBytes bytes{};
    for (std::size_t i = 0; i < kPeerIdBytes; ++i) {
        const int hi = hexValue(hex[i * 2]);
        const int lo = hexValue(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        bytes[i] = static_cast<Byte>((hi << 4) | lo);
    }
    return PeerId{bytes};
}

bool PeerId::isNull() const noexcept
{
    return std::all_of(bytes_.begin(), bytes_.end(), [](Byte b) { return b == 0; });
}

std::string PeerId::toHex() const
{
    std::string out;
    out.reserve(kPeerIdBytes * 2);
    for (const Byte b : bytes_) {
        out.push_back(kHexDigits[(b >> 4) & 0x0F]);
        out.push_back(kHexDigits[b & 0x0F]);
    }
    return out;
}

std::string PeerId::shortLabel() const
{
    // Display only. See kShortLabelChars for why this is not verification.
    const std::string full = toHex();
    return full.substr(full.size() - kShortLabelChars);
}

}  // namespace anytime

// ─────────────────────────────────────────────────────────────────────────────

namespace std {

std::size_t hash<anytime::PeerId>::operator()(const anytime::PeerId& id) const noexcept
{
    // The peer id is already the head of a SHA-256 fingerprint, so its bytes are
    // uniformly distributed — packing them into a size_t is a good hash on its own.
    std::size_t h = 0;
    for (const anytime::Byte b : id.bytes()) {
        h = (h << 8) | static_cast<std::size_t>(b);
    }
    return h;
}

}  // namespace std
