// core/domain/Primitives.h
//
// The bottom of the type stack: scalars and byte views that everything else is
// built from. Depends on nothing but the standard library, so constants/ and
// Enums.h can include it without creating a cycle.
//
// Include order across core/domain:
//
//   Primitives.h                  (no deps)
//   Enums.h                       (no deps)
//   constants/*.h                 -> Primitives.h
//   Identifiers.h                 -> Primitives.h, constants/CryptoConstants.h
//   GlobalStructures.h            -> Identifiers.h, Enums.h
//   Types.h                       -> umbrella over all of the above
//
// Everything outside core/domain includes Types.h and nothing lower.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace anytime {

// ─────────────────────────────────────────────────────────────────────────────
// Bytes
// ─────────────────────────────────────────────────────────────────────────────

using Byte       = std::uint8_t;
using ByteBuffer = std::vector<Byte>;

/// Non-owning view over contiguous bytes. C++17 has no std::span, so we carry a
/// minimal one. The viewed storage must outlive the span.
class ByteSpan {
public:
    constexpr ByteSpan() noexcept = default;
    constexpr ByteSpan(const Byte* data, std::size_t size) noexcept
        : data_(data), size_(size) {}
    ByteSpan(const ByteBuffer& b) noexcept  // NOLINT(google-explicit-constructor)
        : data_(b.data()), size_(b.size()) {}

    constexpr const Byte* data()  const noexcept { return data_; }
    constexpr std::size_t size()  const noexcept { return size_; }
    constexpr bool        empty() const noexcept { return size_ == 0; }

    constexpr const Byte* begin() const noexcept { return data_; }
    constexpr const Byte* end()   const noexcept { return data_ + size_; }

    constexpr Byte operator[](std::size_t i) const noexcept { return data_[i]; }

    /// Returns an empty span when the range is out of bounds. Parsers should
    /// validate lengths up front rather than relying on this.
    ByteSpan subspan(std::size_t offset, std::size_t count) const noexcept {
        if (offset > size_ || count > size_ - offset) return {};
        return ByteSpan{data_ + offset, count};
    }

    ByteBuffer toBuffer() const { return ByteBuffer{begin(), end()}; }

private:
    const Byte* data_ = nullptr;
    std::size_t size_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Time
//
// Milliseconds since the Unix epoch. Always obtained from IClock, never from the
// system clock directly, so tests can advance time without sleeping.
// ─────────────────────────────────────────────────────────────────────────────

using Timestamp = std::int64_t;
inline constexpr Timestamp kNoTimestamp = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Strongly-typed identifier template
//
// Distinct types so a ChatId can never be passed where a MessageId is expected.
// Concrete aliases live in Identifiers.h.
// ─────────────────────────────────────────────────────────────────────────────

template <typename Tag>
class TaggedId {
public:
    TaggedId() = default;
    explicit TaggedId(std::string value) : value_(std::move(value)) {}

    const std::string& value()  const noexcept { return value_; }
    bool               isNull() const noexcept { return value_.empty(); }

    friend bool operator==(const TaggedId& a, const TaggedId& b) noexcept {
        return a.value_ == b.value_;
    }
    friend bool operator!=(const TaggedId& a, const TaggedId& b) noexcept { return !(a == b); }
    friend bool operator<(const TaggedId& a, const TaggedId& b) noexcept {
        return a.value_ < b.value_;
    }

private:
    std::string value_;
};

}  // namespace anytime
