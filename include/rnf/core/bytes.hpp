// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Bounds checked little-endian byte coding. Every value written by the runtime
// is written through ByteWriter and read back through ByteReader, so that the
// canonical encoding of a snapshot is a pure function of its contents and a
// hostile peer cannot make the decoder allocate or read out of bounds.

#ifndef RNF_CORE_BYTES_HPP
#define RNF_CORE_BYTES_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "rnf/core/hash.hpp"
#include "rnf/core/id.hpp"

namespace rnf {

/// Maximum length of a bounded text field, in bytes.
inline constexpr std::size_t kMaxTextField = 256;
/// Maximum length of a bounded opaque blob, in bytes.
inline constexpr std::size_t kMaxBlobField = 4096;
/// Default hard cap on an encoded message.
inline constexpr std::size_t kDefaultEncodingLimit = 1U << 20;  // 1 MiB

/// True when the byte range is well-formed UTF-8: no overlong forms, no
/// surrogates, no code points above U+10FFFF, and no embedded NUL.
[[nodiscard]] bool is_valid_utf8(ByteSpan data) noexcept;

/// Strict UTF-8 validation with an explanation of the first failure.
[[nodiscard]] Status validate_utf8(ByteSpan data);

/// Append-only canonical encoder. Methods never throw; once the configured
/// limit is exceeded the writer latches a failure that ok() reports.
class ByteWriter {
public:
    explicit ByteWriter(std::size_t limit = kDefaultEncodingLimit) : limit_(limit) {
        buffer_.reserve(limit < 4096 ? limit : 4096);
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<std::uint8_t> take() noexcept { return std::move(buffer_); }
    [[nodiscard]] ByteSpan span() const noexcept { return ByteSpan(buffer_.data(), buffer_.size()); }
    void clear() noexcept {
        buffer_.clear();
        ok_ = true;
    }

    void u8(std::uint8_t v) { raw(&v, 1); }
    void u16(std::uint16_t v) {
        std::uint8_t tmp[2];
        tmp[0] = static_cast<std::uint8_t>(v & 0xFFU);
        tmp[1] = static_cast<std::uint8_t>((v >> 8U) & 0xFFU);
        raw(tmp, 2);
    }
    void u32(std::uint32_t v) {
        std::uint8_t tmp[4];
        for (int i = 0; i < 4; ++i) {
            tmp[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU);
        }
        raw(tmp, 4);
    }
    void u64(std::uint64_t v) {
        std::uint8_t tmp[8];
        for (int i = 0; i < 8; ++i) {
            tmp[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU);
        }
        raw(tmp, 8);
    }
    void boolean(bool v) { u8(v ? 1U : 0U); }
    void raw_bytes(ByteSpan data) { raw(data.data(), data.size()); }

    /// Length-prefixed blob with an explicit maximum.
    void blob(ByteSpan data, std::size_t max_len = kMaxBlobField) {
        if (data.size() > max_len || data.size() > 0xFFFFFFFFULL) {
            ok_ = false;
            return;
        }
        u32(static_cast<std::uint32_t>(data.size()));
        raw(data.data(), data.size());
    }

    /// Length-prefixed UTF-8 text. Invalid text latches a failure.
    void text(std::string_view value, std::size_t max_len = kMaxTextField) {
        const ByteSpan bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
        if (!is_valid_utf8(bytes)) {
            ok_ = false;
            return;
        }
        blob(bytes, max_len);
    }

    void digest(const Digest& d) { raw(d.bytes.data(), d.bytes.size()); }

    template <class Id>
    void id(Id value) {
        u64(value.value());
    }

private:
    void raw(const void* src, std::size_t n) {
        if (!ok_ || n > limit_ - buffer_.size()) {
            ok_ = false;
            return;
        }
        const auto* p = static_cast<const std::uint8_t*>(src);
        buffer_.insert(buffer_.end(), p, p + n);
    }

    std::vector<std::uint8_t> buffer_;
    std::size_t limit_;
    bool ok_ = true;
};

/// Bounds checked canonical decoder. Every read is validated before any
/// allocation, so a hostile length prefix cannot cause a large allocation.
class ByteReader {
public:
    explicit ByteReader(ByteSpan data) noexcept : data_(data) {}

    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
    [[nodiscard]] std::size_t position() const noexcept { return offset_; }
    [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

    bool u8(std::uint8_t& out) noexcept {
        if (remaining() < 1) {
            return false;
        }
        out = data_[offset_];
        offset_ += 1;
        return true;
    }
    bool u16(std::uint16_t& out) noexcept {
        if (remaining() < 2) {
            return false;
        }
        out = static_cast<std::uint16_t>(data_[offset_]) |
              static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1]) << 8U);
        offset_ += 2;
        return true;
    }
    bool u32(std::uint32_t& out) noexcept {
        if (remaining() < 4) {
            return false;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(i)])
                 << (8 * i);
        }
        out = v;
        offset_ += 4;
        return true;
    }
    bool u64(std::uint64_t& out) noexcept {
        if (remaining() < 8) {
            return false;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(i)])
                 << (8 * i);
        }
        out = v;
        offset_ += 8;
        return true;
    }
    bool boolean(bool& out) noexcept {
        std::uint8_t v = 0;
        if (!u8(v) || v > 1) {
            return false;
        }
        out = v != 0;
        return true;
    }
    bool raw_bytes(std::size_t n, ByteSpan& out) noexcept {
        if (remaining() < n) {
            return false;
        }
        out = data_.subspan(offset_, n);
        offset_ += n;
        return true;
    }
    /// Reads a length prefix, validates it against both the declared maximum
    /// and the bytes actually available, and only then yields the view.
    bool blob(std::size_t max_len, ByteSpan& out) noexcept {
        std::uint32_t len = 0;
        if (!u32(len)) {
            return false;
        }
        if (len > max_len || remaining() < len) {
            return false;
        }
        return raw_bytes(len, out);
    }
    bool text(std::size_t max_len, std::string& out) {
        ByteSpan view;
        if (!blob(max_len, view)) {
            return false;
        }
        if (!is_valid_utf8(view)) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(view.data()), view.size());
        return true;
    }
    bool digest(Digest& out) noexcept {
        ByteSpan view;
        if (!raw_bytes(out.bytes.size(), view)) {
            return false;
        }
        std::memcpy(out.bytes.data(), view.data(), out.bytes.size());
        return true;
    }
    template <class Id>
    bool id(Id& out) noexcept {
        std::uint64_t v = 0;
        if (!u64(v)) {
            return false;
        }
        out = Id(v);
        return true;
    }
    /// Read a container count and reject anything above the caller's bound
    /// before the caller allocates.
    bool count(std::uint32_t& out, std::uint32_t max_count) noexcept {
        if (!u32(out)) {
            return false;
        }
        return out <= max_count;
    }

private:
    ByteSpan data_;
    std::size_t offset_ = 0;
};

}  // namespace rnf

#endif  // RNF_CORE_BYTES_HPP
