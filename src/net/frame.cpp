// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/net/frame.hpp"

#include <cstring>

#include "rnf/core/hash.hpp"

namespace rnf {
namespace {

void store_u16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFU);
    p[1] = static_cast<std::uint8_t>((v >> 8U) & 0xFFU);
}

void store_u32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU);
    }
}

void store_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU);
    }
}

std::uint16_t load_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8U));
}

std::uint32_t load_u32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    }
    return v;
}

std::uint64_t load_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

}  // namespace

Status encode_frame(const FrameHeader& header, ByteSpan payload, std::vector<std::uint8_t>& out,
                    std::uint32_t max_payload) {
    if (payload.size() > max_payload) {
        return Status(StatusCode::kOversizeField, "frame payload exceeds the negotiated bound");
    }
    if (payload.size() > kHardMaxFramePayload) {
        return Status(StatusCode::kOversizeField, "frame payload exceeds the hard bound");
    }
    out.resize(kFrameHeaderSize + payload.size());
    std::uint8_t* p = out.data();
    store_u32(p, header.magic);
    store_u16(p + 4, header.version);
    store_u16(p + 6, header.type);
    store_u32(p + 8, static_cast<std::uint32_t>(payload.size()));
    store_u32(p + 12, crc32c(payload));
    store_u64(p + 16, header.correlation);
    if (!payload.empty()) {
        std::memcpy(p + kFrameHeaderSize, payload.data(), payload.size());
    }
    return Status{};
}

Status decode_frame_header(ByteSpan bytes, std::uint32_t max_payload,
                           std::uint16_t expected_version, FrameHeader& out) {
    if (bytes.size() < kFrameHeaderSize) {
        return Status(StatusCode::kTruncated, "frame header is incomplete");
    }
    const std::uint8_t* p = bytes.data();
    out.magic = load_u32(p);
    out.version = load_u16(p + 4);
    out.type = load_u16(p + 6);
    out.length = load_u32(p + 8);
    out.crc = load_u32(p + 12);
    out.correlation = load_u64(p + 16);

    if (out.magic != kFrameMagic) {
        return Status(StatusCode::kMalformedEncoding, "frame magic does not match");
    }
    if (out.version != expected_version) {
        return Status(StatusCode::kIncompatibleVersion,
                      "frame declares protocol version " + std::to_string(out.version));
    }
    if (out.length > max_payload || out.length > kHardMaxFramePayload) {
        return Status(StatusCode::kOversizeField,
                      "frame declares " + std::to_string(out.length) +
                          " payload bytes, above the negotiated bound");
    }
    if (!is_known_message_type(out.type)) {
        return Status(StatusCode::kUnsupported,
                      "frame declares unknown message type " + std::to_string(out.type));
    }
    return Status{};
}

}  // namespace rnf
