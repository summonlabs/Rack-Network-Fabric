// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Core primitive tests: hashing against published vectors, checksum, checked
// arithmetic, UTF-8 validation, the canonical byte codec and the deterministic
// generator.

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "rnf/core/bytes.hpp"
#include "rnf/core/checked.hpp"
#include "rnf/core/hash.hpp"
#include "rnf/core/id.hpp"
#include "rnf/core/status.hpp"
#include "rnf/model/types.hpp"
#include "tests/support/harness.hpp"

using namespace rnf;

namespace {

std::string hex_of(const Digest& digest) {
    return digest.to_hex();
}

bool utf8_ok(const char* text) {
    const std::size_t length = std::char_traits<char>::length(text);
    return is_valid_utf8(
        ByteSpan(reinterpret_cast<const std::uint8_t*>(text), length));
}

}  // namespace

RNF_TEST(core, blake2s_known_vectors) {
    RNF_CHECK_EQ(hex_of(Blake2s256::hash(std::string_view(""))),
                 std::string("69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9"));
    RNF_CHECK_EQ(hex_of(Blake2s256::hash(std::string_view("abc"))),
                 std::string("508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982"));
    RNF_CHECK_EQ(
        hex_of(Blake2s256::hash(std::string_view("The quick brown fox jumps over the lazy dog"))),
        std::string("606beeec743ccbeff6cbcdf5d5302aa855c256c29b88c8ed331ea1a6bf3c8812"));
}

RNF_TEST(core, blake2s_incremental_matches_one_shot) {
    std::string payload;
    for (int i = 0; i < 5000; ++i) {
        payload.push_back(static_cast<char>('a' + (i % 26)));
    }
    const Digest one_shot = Blake2s256::hash(std::string_view(payload));
    for (std::size_t chunk = 1; chunk <= 130; chunk += 7) {
        Blake2s256 hasher;
        std::size_t offset = 0;
        while (offset < payload.size()) {
            const std::size_t take = std::min(chunk, payload.size() - offset);
            hasher.update(std::string_view(payload.data() + offset, take));
            offset += take;
        }
        RNF_CHECK(hasher.final() == one_shot);
    }
    // Block aligned inputs exercise the rule that the final block is only
    // compressed once the writer knows no more input follows.
    const std::array<std::size_t, 8> sizes = {0, 1, 63, 64, 65, 127, 128, 129};
    for (std::size_t size : sizes) {
        const std::string exact(size, 'x');
        Blake2s256 streamed;
        for (char c : exact) {
            streamed.update(std::string_view(&c, 1));
        }
        RNF_CHECK(streamed.final() == Blake2s256::hash(std::string_view(exact)));
    }
}

RNF_TEST(core, crc32c_known_vectors) {
    RNF_CHECK_EQ(crc32c(std::string_view("123456789")), 0xE3069283U);
    RNF_CHECK_EQ(crc32c(std::string_view("")), 0U);
    RNF_CHECK_EQ(crc32c(std::string_view("a")), 0xC1D04330U);
    const std::uint32_t first = crc32c(std::string_view("hello "));
    const std::uint32_t combined =
        crc32c_extend(first, ByteSpan(reinterpret_cast<const std::uint8_t*>("world"), 5));
    RNF_CHECK_EQ(combined, crc32c(std::string_view("hello world")));
}

RNF_TEST(core, checked_arithmetic) {
    std::uint64_t out = 0;
    RNF_CHECK(checked_add_u64(1, 2, out));
    RNF_CHECK_EQ(out, 3U);
    RNF_CHECK(!checked_add_u64(std::numeric_limits<std::uint64_t>::max(), 1, out));
    RNF_CHECK(checked_sub_u64(5, 5, out));
    RNF_CHECK_EQ(out, 0U);
    RNF_CHECK(!checked_sub_u64(0, 1, out));
    RNF_CHECK(checked_mul_u64(0, std::numeric_limits<std::uint64_t>::max(), out));
    RNF_CHECK_EQ(out, 0U);
    RNF_CHECK(!checked_mul_u64(std::numeric_limits<std::uint64_t>::max(), 2, out));
    RNF_CHECK_EQ(saturating_add_u64(std::numeric_limits<std::uint64_t>::max(), 5),
                 std::numeric_limits<std::uint64_t>::max());
    std::uint32_t narrow = 0;
    RNF_CHECK(narrow_u64_to_u32(4294967295ULL, narrow));
    RNF_CHECK(!narrow_u64_to_u32(4294967296ULL, narrow));
    std::uint16_t small = 0;
    RNF_CHECK(narrow_u64_to_u16(65535ULL, small));
    RNF_CHECK(!narrow_u64_to_u16(65536ULL, small));
}

RNF_TEST(core, status_vocabulary_is_distinct) {
    const std::array<StatusCode, 10> distinct = {
        StatusCode::kUnknown,       StatusCode::kUnsupported, StatusCode::kStaleGeneration,
        StatusCode::kConflict,      StatusCode::kIncomplete,  StatusCode::kIndeterminate,
        StatusCode::kRefused,       StatusCode::kCancelled,   StatusCode::kInvalidArgument,
        StatusCode::kIntegrityFailure,
    };
    for (std::size_t i = 0; i < distinct.size(); ++i) {
        RNF_CHECK(classify(distinct[i]) != StatusClass::kOk);
        for (std::size_t j = i + 1; j < distinct.size(); ++j) {
            RNF_CHECK(!(to_string(distinct[i]) == to_string(distinct[j])));
        }
    }
    RNF_CHECK(classify(StatusCode::kOk) == StatusClass::kOk);
    RNF_CHECK_EQ(std::string(to_string(StatusClass::kUnknown)), std::string("UNKNOWN"));
    RNF_CHECK_EQ(std::string(to_string(StatusClass::kStale)), std::string("STALE"));
    RNF_CHECK_EQ(std::string(to_string(StatusClass::kIncomplete)), std::string("INCOMPLETE"));
    RNF_CHECK_EQ(std::string(to_string(StatusClass::kIndeterminate)),
                 std::string("INDETERMINATE"));
    RNF_CHECK_EQ(std::string(to_string(StatusClass::kCancelled)), std::string("CANCELLED"));
    RNF_CHECK_EQ(std::string(to_string(StatusClass::kRefused)), std::string("REFUSED"));
    RNF_CHECK_EQ(std::string(to_string(StatusClass::kUnsupported)), std::string("UNSUPPORTED"));
    RNF_CHECK_EQ(std::string(to_string(StatusClass::kInvalid)), std::string("INVALID"));
}

RNF_TEST(core, utf8_validation) {
    RNF_CHECK(utf8_ok(""));
    RNF_CHECK(utf8_ok("plain ascii"));
    RNF_CHECK(utf8_ok("\xC3\xA9"));
    RNF_CHECK(utf8_ok("\xE2\x82\xAC"));
    RNF_CHECK(utf8_ok("\xF0\x9F\x98\x80"));
    RNF_CHECK(!utf8_ok("\xC3"));
    RNF_CHECK(!utf8_ok("\xC0\x80"));
    RNF_CHECK(!utf8_ok("\xE0\x80\x80"));
    RNF_CHECK(!utf8_ok("\xED\xA0\x80"));
    RNF_CHECK(!utf8_ok("\xF5\x80\x80\x80"));
    RNF_CHECK(!utf8_ok("\x80"));
    // An embedded NUL is rejected even though it is valid UTF-8, because every
    // text field in the runtime is required to be C-string safe.
    const char embedded_nul[] = {'a', '\0', 'b'};
    RNF_CHECK(!is_valid_utf8(ByteSpan(reinterpret_cast<const std::uint8_t*>(embedded_nul), 3)));
}

RNF_TEST(core, byte_codec_roundtrip_and_bounds) {
    ByteWriter writer(256);
    writer.u8(0xAB);
    writer.u16(0x1234);
    writer.u32(0xDEADBEEF);
    writer.u64(0x0123456789ABCDEFULL);
    writer.boolean(true);
    writer.text("hello", 16);
    writer.raw_bytes(ByteSpan(reinterpret_cast<const std::uint8_t*>("raw"), 3));
    RNF_CHECK(writer.ok());

    ByteReader reader(writer.span());
    std::uint8_t a = 0;
    std::uint16_t b = 0;
    std::uint32_t c = 0;
    std::uint64_t d = 0;
    bool e = false;
    std::string text;
    ByteSpan raw;
    RNF_CHECK(reader.u8(a));
    RNF_CHECK(reader.u16(b));
    RNF_CHECK(reader.u32(c));
    RNF_CHECK(reader.u64(d));
    RNF_CHECK(reader.boolean(e));
    RNF_CHECK(reader.text(16, text));
    RNF_CHECK(reader.raw_bytes(3, raw));
    RNF_CHECK_EQ(a, static_cast<std::uint8_t>(0xAB));
    RNF_CHECK_EQ(b, static_cast<std::uint16_t>(0x1234));
    RNF_CHECK_EQ(c, 0xDEADBEEFU);
    RNF_CHECK_EQ(d, 0x0123456789ABCDEFULL);
    RNF_CHECK(e);
    RNF_CHECK_EQ(text, std::string("hello"));
    RNF_CHECK(reader.at_end());

    // A length prefix larger than the remaining bytes is refused without
    // reading out of bounds.
    ByteWriter hostile(64);
    hostile.u32(0xFFFFFFFFU);
    ByteReader hostile_reader(hostile.span());
    ByteSpan view;
    RNF_CHECK(!hostile_reader.blob(1U << 20, view));

    // A length prefix larger than the caller's bound is refused even when the
    // bytes really are present.
    ByteWriter oversized(64);
    oversized.u32(32);
    const std::uint8_t filler[32] = {};
    oversized.raw_bytes(ByteSpan(filler, 32));
    ByteReader oversized_reader(oversized.span());
    RNF_CHECK(!oversized_reader.blob(16, view));

    // Writing past the configured limit latches a failure instead of growing.
    ByteWriter bounded(4);
    bounded.u64(1);
    RNF_CHECK(!bounded.ok());
}

RNF_TEST(core, strong_id_and_digest_text) {
    RNF_CHECK_EQ(DeviceId(255).to_hex(), std::string("00000000000000ff"));
    RNF_CHECK(!DeviceId(0).is_set());
    RNF_CHECK(DeviceId(1) < DeviceId(2));
    RNF_CHECK(DeviceId(0) < DeviceId(1));

    Digest digest;
    RNF_CHECK(digest.is_zero());
    for (std::size_t i = 0; i < digest.bytes.size(); ++i) {
        digest.bytes[i] = static_cast<std::uint8_t>(i);
    }
    RNF_CHECK(!digest.is_zero());
    const std::string text = digest.to_hex();
    RNF_CHECK_EQ(text.size(), 64U);
    RNF_REQUIRE_VALUE(parsed, Digest::from_hex(text));
    RNF_CHECK(parsed == digest);
    RNF_REQUIRE_CODE(Digest::from_hex("short"), StatusCode::kMalformedEncoding);
    RNF_REQUIRE_CODE(Digest::from_hex(std::string(64, 'z')), StatusCode::kMalformedEncoding);
}

RNF_TEST(core, splitmix64_is_deterministic) {
    SplitMix64 a(42);
    SplitMix64 b(42);
    for (int i = 0; i < 1000; ++i) {
        RNF_CHECK_EQ(a.next(), b.next());
    }
    SplitMix64 c(43);
    SplitMix64 d(42);
    RNF_CHECK(!(c.next() == d.next()));
    for (int i = 0; i < 100; ++i) {
        RNF_CHECK(SplitMix64(static_cast<std::uint64_t>(i) + 1).bounded(10) < 10);
    }
    RNF_CHECK_EQ(SplitMix64(7).bounded(0), 0U);
}

RNF_TEST(core, resource_ref_text_roundtrip) {
    const ResourceRef original{ResourceKind::kPort, 42};
    RNF_REQUIRE_VALUE(parsed, parse_resource_ref(original.to_text()));
    RNF_CHECK(parsed == original);
    RNF_REQUIRE_CODE(parse_resource_ref("port"), StatusCode::kMalformedEncoding);
    RNF_REQUIRE_CODE(parse_resource_ref("nonsense:1"), StatusCode::kMalformedEncoding);
    RNF_REQUIRE_CODE(parse_resource_ref("port:0"), StatusCode::kInvalidIdentity);
    RNF_REQUIRE_CODE(parse_resource_ref("port:abc"), StatusCode::kInvalidIdentity);
}
