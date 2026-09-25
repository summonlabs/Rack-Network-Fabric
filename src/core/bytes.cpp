// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/core/bytes.hpp"

namespace rnf {

bool is_valid_utf8(ByteSpan data) noexcept {
    std::size_t i = 0;
    const std::size_t n = data.size();
    while (i < n) {
        const std::uint8_t b0 = data[i];
        if (b0 == 0x00U) {
            return false;  // embedded NUL is rejected: text fields are C-string safe
        }
        if (b0 < 0x80U) {
            i += 1;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t cp = 0;
        std::uint32_t lowest = 0;
        if ((b0 & 0xE0U) == 0xC0U) {
            extra = 1;
            cp = b0 & 0x1FU;
            lowest = 0x80U;
        } else if ((b0 & 0xF0U) == 0xE0U) {
            extra = 2;
            cp = b0 & 0x0FU;
            lowest = 0x800U;
        } else if ((b0 & 0xF8U) == 0xF0U) {
            extra = 3;
            cp = b0 & 0x07U;
            lowest = 0x10000U;
        } else {
            return false;  // continuation byte or 5/6 byte form
        }
        if (i + extra >= n) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const std::uint8_t bk = data[i + k];
            if ((bk & 0xC0U) != 0x80U) {
                return false;
            }
            cp = (cp << 6U) | (bk & 0x3FU);
        }
        if (cp < lowest) {
            return false;  // overlong encoding
        }
        if (cp > 0x10FFFFU) {
            return false;
        }
        if (cp >= 0xD800U && cp <= 0xDFFFU) {
            return false;  // surrogate half
        }
        i += extra + 1;
    }
    return true;
}

Status validate_utf8(ByteSpan data) {
    if (is_valid_utf8(data)) {
        return Status{};
    }
    return Status(StatusCode::kInvalidUnicode, "field is not well-formed UTF-8");
}

}  // namespace rnf
