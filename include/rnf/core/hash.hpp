// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Self contained BLAKE2s-256 (RFC 7693) and CRC-32C (Castagnoli) for content
// addressing and persistence integrity. No third-party code.

#ifndef RNF_CORE_HASH_HPP
#define RNF_CORE_HASH_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "rnf/core/id.hpp"

namespace rnf {

using ByteSpan = std::span<const std::uint8_t>;

/// Incremental BLAKE2s-256.
class Blake2s256 {
public:
    static constexpr std::size_t kDigestBytes = 32;
    static constexpr std::size_t kBlockBytes = 64;

    Blake2s256() noexcept;

    /// Reset to the initial state, optionally keyed (key length <= 32).
    void reset(ByteSpan key = {}) noexcept;

    /// Absorb bytes. Thread-confined; not safe for concurrent use.
    void update(ByteSpan data) noexcept;
    void update(std::string_view text) noexcept {
        update(ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

    /// Absorb a 64 bit value in little-endian order.
    void update_le64(std::uint64_t value) noexcept {
        std::uint8_t buffer[8];
        for (int i = 0; i < 8; ++i) {
            buffer[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU);
        }
        update(ByteSpan(buffer, sizeof(buffer)));
    }

    /// Produce the digest and leave the object reusable via reset().
    [[nodiscard]] Digest final();

    /// One-shot convenience.
    [[nodiscard]] static Digest hash(ByteSpan data) noexcept;
    [[nodiscard]] static Digest hash(std::string_view text) noexcept;

private:
    void compress(const std::uint8_t* block, bool last) noexcept;

    std::uint32_t h_[8]{};
    std::uint8_t buffer_[kBlockBytes]{};
    std::uint64_t counter_ = 0;
    std::size_t buffer_len_ = 0;
};

/// CRC-32C (Castagnoli polynomial, reflected). Used for record integrity.
[[nodiscard]] std::uint32_t crc32c(ByteSpan data) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::string_view text) noexcept;
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t seed, ByteSpan data) noexcept;

}  // namespace rnf

#endif  // RNF_CORE_HASH_HPP
