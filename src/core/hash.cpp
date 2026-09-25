// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/core/hash.hpp"

#include <cstring>

namespace rnf {
namespace {

// --- BLAKE2s constants (RFC 7693) ------------------------------------------

constexpr std::uint32_t kIV[8] = {
    0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
    0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
};

constexpr std::uint8_t kSigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

constexpr std::uint32_t rotr32(std::uint32_t x, unsigned n) noexcept {
    return (x >> n) | (x << (32U - n));
}

constexpr std::uint32_t load32_le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
}

void store32_le(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 8U) & 0xFFU);
    p[2] = static_cast<std::uint8_t>((v >> 16U) & 0xFFU);
    p[3] = static_cast<std::uint8_t>((v >> 24U) & 0xFFU);
}

inline void mix(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c, std::uint32_t& d,
                std::uint32_t x, std::uint32_t y) noexcept {
    a = a + b + x;
    d = rotr32(d ^ a, 16);
    c = c + d;
    b = rotr32(b ^ c, 12);
    a = a + b + y;
    d = rotr32(d ^ a, 8);
    c = c + d;
    b = rotr32(b ^ c, 7);
}

// --- CRC-32C table ---------------------------------------------------------

struct Crc32cTable {
    std::uint32_t entries[256];
    constexpr Crc32cTable() noexcept : entries{} {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = ((crc & 1U) != 0U) ? ((crc >> 1U) ^ 0x82F63B78U) : (crc >> 1U);
            }
            entries[i] = crc;
        }
    }
};

constexpr Crc32cTable kCrc32c{};

}  // namespace

Blake2s256::Blake2s256() noexcept {
    reset();
}

void Blake2s256::reset(ByteSpan key) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        h_[i] = kIV[i];
    }
    // Parameter block, XORed into the first state word: digest length 32,
    // key length, fanout 1, depth 1. Omitting the digest length byte produces a
    // hash that is self consistent but is not BLAKE2s, which is exactly what
    // the RFC 7693 vectors in the test suite exist to catch.
    h_[0] ^= 0x01010020U ^ (static_cast<std::uint32_t>(key.size()) << 8U);
    std::memset(buffer_, 0, sizeof(buffer_));
    counter_ = 0;
    buffer_len_ = 0;

    if (!key.empty()) {
        std::uint8_t block[kBlockBytes] = {};
        const std::size_t n = key.size() < kBlockBytes ? key.size() : kBlockBytes;
        std::memcpy(block, key.data(), n);
        // A keyed hash absorbs the padded key block as ordinary input.
        update(ByteSpan(block, kBlockBytes));
    }
}

void Blake2s256::compress(const std::uint8_t* block, bool last) noexcept {
    std::uint32_t m[16];
    for (std::size_t i = 0; i < 16; ++i) {
        m[i] = load32_le(block + i * 4);
    }

    std::uint32_t v[16];
    for (std::size_t i = 0; i < 8; ++i) {
        v[i] = h_[i];
        v[i + 8] = kIV[i];
    }
    v[12] ^= static_cast<std::uint32_t>(counter_ & 0xFFFFFFFFULL);
    v[13] ^= static_cast<std::uint32_t>(counter_ >> 32U);
    if (last) {
        // Finalisation flag f0 = 0xFFFFFFFF.
        v[14] = ~v[14];
    }

    for (std::size_t round = 0; round < 10; ++round) {
        const std::uint8_t* s = kSigma[round];
        mix(v[0], v[4], v[8], v[12], m[s[0]], m[s[1]]);
        mix(v[1], v[5], v[9], v[13], m[s[2]], m[s[3]]);
        mix(v[2], v[6], v[10], v[14], m[s[4]], m[s[5]]);
        mix(v[3], v[7], v[11], v[15], m[s[6]], m[s[7]]);
        mix(v[0], v[5], v[10], v[15], m[s[8]], m[s[9]]);
        mix(v[1], v[6], v[11], v[12], m[s[10]], m[s[11]]);
        mix(v[2], v[7], v[8], v[13], m[s[12]], m[s[13]]);
        mix(v[3], v[4], v[9], v[14], m[s[14]], m[s[15]]);
    }
    for (std::size_t i = 0; i < 8; ++i) {
        h_[i] ^= v[i] ^ v[i + 8];
    }
}

void Blake2s256::update(ByteSpan data) noexcept {
    const std::uint8_t* p = data.data();
    std::size_t remaining = data.size();

    // A full buffer is only compressed once we know more input follows: the
    // final block must be compressed with the finalisation flag set.
    while (remaining > 0) {
        if (buffer_len_ == kBlockBytes) {
            counter_ += kBlockBytes;
            compress(buffer_, false);
            buffer_len_ = 0;
        }
        const std::size_t space = kBlockBytes - buffer_len_;
        const std::size_t take = remaining < space ? remaining : space;
        std::memcpy(buffer_ + buffer_len_, p, take);
        buffer_len_ += take;
        p += take;
        remaining -= take;
    }
}

Digest Blake2s256::final() {
    counter_ += static_cast<std::uint64_t>(buffer_len_);
    std::memset(buffer_ + buffer_len_, 0, kBlockBytes - buffer_len_);
    compress(buffer_, true);

    Digest out;
    for (std::size_t i = 0; i < 8; ++i) {
        store32_le(out.bytes.data() + i * 4, h_[i]);
    }
    return out;
}

Digest Blake2s256::hash(ByteSpan data) noexcept {
    Blake2s256 h;
    h.update(data);
    return h.final();
}

Digest Blake2s256::hash(std::string_view text) noexcept {
    return hash(ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::uint32_t crc32c_extend(std::uint32_t seed, ByteSpan data) noexcept {
    std::uint32_t crc = ~seed;
    for (std::uint8_t byte : data) {
        crc = kCrc32c.entries[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

std::uint32_t crc32c(ByteSpan data) noexcept {
    return crc32c_extend(0U, data);
}

std::uint32_t crc32c(std::string_view text) noexcept {
    return crc32c(ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

}  // namespace rnf
