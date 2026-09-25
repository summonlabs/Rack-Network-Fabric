// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Checked integer arithmetic. Every capacity computation in the runtime goes
// through these helpers so that an overflow becomes a typed refusal instead of
// a wrapped value that silently under-reports usage.

#ifndef RNF_CORE_CHECKED_HPP
#define RNF_CORE_CHECKED_HPP

#include <cstdint>
#include <limits>

#include "rnf/core/status.hpp"

namespace rnf {

[[nodiscard]] constexpr bool checked_add_u64(std::uint64_t a, std::uint64_t b,
                                             std::uint64_t& out) noexcept {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) {
        return false;
    }
    out = a + b;
    return true;
}

[[nodiscard]] constexpr bool checked_sub_u64(std::uint64_t a, std::uint64_t b,
                                             std::uint64_t& out) noexcept {
    if (b > a) {
        return false;
    }
    out = a - b;
    return true;
}

[[nodiscard]] constexpr bool checked_mul_u64(std::uint64_t a, std::uint64_t b,
                                             std::uint64_t& out) noexcept {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    if (a > std::numeric_limits<std::uint64_t>::max() / b) {
        return false;
    }
    out = a * b;
    return true;
}

[[nodiscard]] constexpr bool checked_add_u32(std::uint32_t a, std::uint32_t b,
                                             std::uint32_t& out) noexcept {
    if (a > std::numeric_limits<std::uint32_t>::max() - b) {
        return false;
    }
    out = a + b;
    return true;
}

/// Saturating add used only for diagnostics counters where wrapping would be a
/// worse failure mode than pinning at the maximum.
[[nodiscard]] constexpr std::uint64_t saturating_add_u64(std::uint64_t a,
                                                         std::uint64_t b) noexcept {
    std::uint64_t out = 0;
    return checked_add_u64(a, b, out) ? out : std::numeric_limits<std::uint64_t>::max();
}

/// Narrowing conversion that refuses instead of truncating.
[[nodiscard]] constexpr bool narrow_u64_to_u32(std::uint64_t value,
                                               std::uint32_t& out) noexcept {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

[[nodiscard]] constexpr bool narrow_u64_to_u16(std::uint64_t value,
                                               std::uint16_t& out) noexcept {
    if (value > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
}

}  // namespace rnf

#endif  // RNF_CORE_CHECKED_HPP
