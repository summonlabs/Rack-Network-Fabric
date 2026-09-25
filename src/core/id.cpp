// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/core/id.hpp"

namespace rnf {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

constexpr std::uint8_t hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<std::uint8_t>(c - 'A' + 10);
    }
    return 0xFFU;
}

}  // namespace

std::string hex64(std::uint64_t value) {
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHexDigits[value & 0xFU];
        value >>= 4U;
    }
    return out;
}

template <class Tag>
std::string StrongId<Tag>::to_hex() const {
    return hex64(value_);
}

// Explicit instantiations for every tag used by the runtime. Keeping this list
// explicit means a new identity type fails to link until it is registered here.
template std::string StrongId<RackIdTag>::to_hex() const;
template std::string StrongId<DeviceIdTag>::to_hex() const;
template std::string StrongId<PortIdTag>::to_hex() const;
template std::string StrongId<LinkIdTag>::to_hex() const;
template std::string StrongId<AttachmentIdTag>::to_hex() const;
template std::string StrongId<GrantIdTag>::to_hex() const;
template std::string StrongId<PrincipalIdTag>::to_hex() const;
template std::string StrongId<SourceIdTag>::to_hex() const;
template std::string StrongId<ObligationIdTag>::to_hex() const;
template std::string StrongId<PathIdTag>::to_hex() const;

std::string RequestId::to_hex() const {
    return hex64(hi) + hex64(lo);
}

bool Digest::is_zero() const noexcept {
    for (std::uint8_t b : bytes) {
        if (b != 0U) {
            return false;
        }
    }
    return true;
}

std::string Digest::to_hex() const {
    std::string out;
    out.reserve(64);
    for (std::uint8_t b : bytes) {
        out.push_back(kHexDigits[(b >> 4U) & 0xFU]);
        out.push_back(kHexDigits[b & 0xFU]);
    }
    return out;
}

Result<Digest> Digest::from_hex(std::string_view text) {
    if (text.size() != 64) {
        return Status(StatusCode::kMalformedEncoding, "digest must be 64 hex characters");
    }
    Digest out;
    for (std::size_t i = 0; i < 32; ++i) {
        const std::uint8_t hi = hex_value(text[i * 2]);
        const std::uint8_t lo = hex_value(text[i * 2 + 1]);
        if (hi == 0xFFU || lo == 0xFFU) {
            return Status(StatusCode::kMalformedEncoding, "digest contains a non-hex character");
        }
        out.bytes[i] = static_cast<std::uint8_t>((hi << 4U) | lo);
    }
    return out;
}

std::uint64_t SplitMix64::next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

std::uint64_t SplitMix64::bounded(std::uint64_t exclusive_upper) noexcept {
    if (exclusive_upper == 0) {
        return 0;
    }
    // Modulo reduction. The tiny modulo bias is irrelevant for test input
    // generation, and the mapping stays deterministic for a given seed, which
    // is what property tests depend on.
    return next() % exclusive_upper;
}

}  // namespace rnf
