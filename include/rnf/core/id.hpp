// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Strongly typed identities. Two identities with different tags are different
// types, so a device id can never be passed where a port id is expected.

#ifndef RNF_CORE_ID_HPP
#define RNF_CORE_ID_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "rnf/core/status.hpp"

namespace rnf {

/// Tag types. Only used to parameterise StrongId.
struct RackIdTag {};
struct DeviceIdTag {};
struct PortIdTag {};
struct LinkIdTag {};
struct AttachmentIdTag {};
struct GrantIdTag {};
struct PrincipalIdTag {};
struct SourceIdTag {};
struct ObligationIdTag {};
struct PathIdTag {};

/// A 64 bit identity carrying a phantom tag.
///
/// The zero value is reserved to mean "unset". Identities are compared and
/// ordered by their numeric value only, which is what makes canonical ordering
/// of identity-keyed containers well defined.
template <class Tag>
class StrongId {
public:
    using rep_type = std::uint64_t;
    using tag_type = Tag;

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(rep_type value) noexcept : value_(value) {}

    [[nodiscard]] constexpr rep_type value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_set() const noexcept { return value_ != 0; }

    friend constexpr bool operator==(StrongId a, StrongId b) noexcept { return a.value_ == b.value_; }
    friend constexpr std::strong_ordering operator<=>(StrongId a, StrongId b) noexcept {
        return a.value_ <=> b.value_;
    }

    /// Lower-case hex without a prefix, zero padded to 16 digits.
    [[nodiscard]] std::string to_hex() const;

private:
    rep_type value_ = 0;
};

using RackId = StrongId<RackIdTag>;
using DeviceId = StrongId<DeviceIdTag>;
using PortId = StrongId<PortIdTag>;
using LinkId = StrongId<LinkIdTag>;
using AttachmentId = StrongId<AttachmentIdTag>;
using GrantId = StrongId<GrantIdTag>;
using PrincipalId = StrongId<PrincipalIdTag>;
using SourceId = StrongId<SourceIdTag>;
using ObligationId = StrongId<ObligationIdTag>;

/// Globally unique path identity, derived from the ordered link sequence.
using PathId = StrongId<PathIdTag>;

/// 128 bit client supplied request identity, used for idempotent mutation.
struct RequestId {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;

    [[nodiscard]] constexpr bool is_set() const noexcept { return hi != 0 || lo != 0; }
    friend constexpr bool operator==(const RequestId&, const RequestId&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const RequestId& a, const RequestId& b) noexcept {
        if (a.hi != b.hi) {
            return a.hi <=> b.hi;
        }
        return a.lo <=> b.lo;
    }
    [[nodiscard]] std::string to_hex() const;
};

/// A 256 bit content address produced by BLAKE2s.
struct Digest {
    std::array<std::uint8_t, 32> bytes{};

    friend constexpr bool operator==(const Digest&, const Digest&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const Digest& a, const Digest& b) noexcept {
        for (std::size_t i = 0; i < a.bytes.size(); ++i) {
            if (a.bytes[i] != b.bytes[i]) {
                return a.bytes[i] <=> b.bytes[i];
            }
        }
        return std::strong_ordering::equal;
    }
    [[nodiscard]] bool is_zero() const noexcept;
    [[nodiscard]] std::string to_hex() const;

    /// Parse 64 hex characters. Returns kInvalidArgument on any other input.
    [[nodiscard]] static Result<Digest> from_hex(std::string_view text);
};

/// Format a 64 bit value as zero padded lower-case hex.
[[nodiscard]] std::string hex64(std::uint64_t value);

/// SplitMix64 - the deterministic generator used by property tests.
class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}
    [[nodiscard]] std::uint64_t next() noexcept;
    [[nodiscard]] std::uint64_t bounded(std::uint64_t exclusive_upper) noexcept;
    [[nodiscard]] bool coin() noexcept { return (next() & 1U) != 0U; }

private:
    std::uint64_t state_;
};

}  // namespace rnf

namespace std {
template <class Tag>
struct hash<rnf::StrongId<Tag>> {
    [[nodiscard]] size_t operator()(rnf::StrongId<Tag> id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value());
    }
};

template <>
struct hash<rnf::RequestId> {
    [[nodiscard]] size_t operator()(const rnf::RequestId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.hi) ^ (std::hash<std::uint64_t>{}(id.lo) << 1U);
    }
};

template <>
struct hash<rnf::Digest> {
    [[nodiscard]] size_t operator()(const rnf::Digest& d) const noexcept {
        size_t h = 1469598103934665603ULL;
        for (std::uint8_t b : d.bytes) {
            h ^= static_cast<size_t>(b);
            h *= 1099511628211ULL;
        }
        return h;
    }
};
}  // namespace std

#endif  // RNF_CORE_ID_HPP
