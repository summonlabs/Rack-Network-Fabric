// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Length-prefixed framing for the daemon protocol. Every field is validated
// before it is used to size an allocation, so a hostile peer cannot make the
// receiver reserve memory it did not ask for.

#ifndef RNF_NET_FRAME_HPP
#define RNF_NET_FRAME_HPP

#include <cstdint>
#include <vector>

#include "rnf/core/bytes.hpp"

namespace rnf {

inline constexpr std::uint32_t kFrameMagic = 0x31464E52U;  // "RNF1" little endian
inline constexpr std::size_t kFrameHeaderSize = 24;

/// Hard ceiling on a single frame payload. A frame larger than this is refused
/// before any buffer is allocated for it.
inline constexpr std::uint32_t kHardMaxFramePayload = 8U << 20;  // 8 MiB
/// Default ceiling negotiated by rnfd.
inline constexpr std::uint32_t kDefaultMaxFramePayload = 1U << 20;  // 1 MiB

struct FrameHeader {
    std::uint32_t magic = kFrameMagic;
    std::uint16_t version = 0;
    std::uint16_t type = 0;
    std::uint32_t length = 0;
    std::uint32_t crc = 0;
    std::uint64_t correlation = 0;

    friend bool operator==(const FrameHeader&, const FrameHeader&) = default;
};

/// Serialise a frame header and payload into out. Returns kOversizeField when
/// the payload exceeds max_payload.
[[nodiscard]] Status encode_frame(const FrameHeader& header, ByteSpan payload,
                                  std::vector<std::uint8_t>& out, std::uint32_t max_payload);

/// Parse and validate a frame header. Validates magic, version, the declared
/// length against max_payload, and that the type is in the accepted set.
[[nodiscard]] Status decode_frame_header(ByteSpan bytes, std::uint32_t max_payload,
                                         std::uint16_t expected_version, FrameHeader& out);

/// True when the type value is a known message type.
[[nodiscard]] bool is_known_message_type(std::uint16_t type) noexcept;

}  // namespace rnf

#endif  // RNF_NET_FRAME_HPP
