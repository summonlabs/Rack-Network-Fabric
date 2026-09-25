// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Persistence tests: real close and reopen, torn tails, corruption in the
// middle of the log, checkpoints, version refusal and exclusive locking.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "rnf/persist/store.hpp"
#include "rnf/version.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/harness.hpp"

using namespace rnf;
using namespace rnf::test;

namespace {

StoreRecord evidence_store_record(const EvidenceRecord& evidence) {
    StoreRecord record;
    record.type = RecordType::kEvidence;
    record.payload = evidence;
    return record;
}

StoreRecord lifecycle_store_record(LifecycleState state, std::uint64_t generation) {
    StoreRecord record;
    record.type = RecordType::kLifecycle;
    LifecycleRecord body;
    body.state = state;
    body.generation = TopologyGeneration{generation};
    record.payload = body;
    return record;
}

std::size_t count_evidence(const std::vector<StoreRecord>& records) {
    std::size_t count = 0;
    for (const StoreRecord& record : records) {
        if (record.type == RecordType::kEvidence) {
            ++count;
        }
    }
    return count;
}

std::uintmax_t size_of_file(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::file_size(path, ec);
}

void truncate_file(const std::filesystem::path& path, std::uintmax_t keep) {
    std::vector<char> bytes;
    {
        std::ifstream input(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    bytes.resize(static_cast<std::size_t>(keep));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void flip_byte(const std::filesystem::path& path, std::uintmax_t offset) {
    std::vector<char> bytes;
    {
        std::ifstream input(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    RNF_REQUIRE(offset < bytes.size());
    bytes[static_cast<std::size_t>(offset)] =
        static_cast<char>(bytes[static_cast<std::size_t>(offset)] ^ 0x5A);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

StoreOptions default_options() {
    StoreOptions options;
    options.fsync_on_append = true;
    return options;
}

}  // namespace

RNF_TEST(persist, fresh_store_then_close_and_reopen) {
    const std::filesystem::path directory = make_temp_directory("store-basic");
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
        RNF_CHECK(store->recovery().fresh_store);
        RNF_CHECK(!store->recovery().clean_shutdown);
        RNF_CHECK_EQ(store->recovered().records.size(), 0U);
        RNF_REQUIRE_VALUE(incarnation, store->next_incarnation());
        RNF_CHECK_EQ(incarnation.value, 1U);
        RNF_REQUIRE_OK(store->bind_rack(kRack));
        for (const EvidenceRecord& record : linear_rack(0)) {
            RNF_REQUIRE_OK(store->append(evidence_store_record(record)));
        }
        RNF_REQUIRE_OK(store->append(lifecycle_store_record(LifecycleState::kActive, 0)));
        RNF_REQUIRE_OK(store->mark_clean_shutdown(1234));
    }
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
        RNF_CHECK(!store->recovery().fresh_store);
        RNF_CHECK(store->recovery().clean_shutdown);
        RNF_CHECK(!store->recovery().truncated_tail);
        RNF_CHECK(!store->recovery().corrupt_record);
        RNF_CHECK_EQ(store->recovered().records.size(), linear_rack(0).size() + 1);
        RNF_CHECK_EQ(count_evidence(store->recovered().records), linear_rack(0).size());
        RNF_CHECK(store->bound_rack() == kRack);
        // The incarnation counter keeps moving forward across restarts.
        RNF_REQUIRE_VALUE(incarnation, store->next_incarnation());
        RNF_CHECK_EQ(incarnation.value, 2U);
        RNF_REQUIRE_OK(store->mark_clean_shutdown(5678));
    }
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
        RNF_REQUIRE_VALUE(incarnation, store->next_incarnation());
        RNF_CHECK_EQ(incarnation.value, 3U);
    }
}

RNF_TEST(persist, torn_tail_is_discarded_and_reported) {
    const std::filesystem::path directory = make_temp_directory("store-torn");
    const std::vector<EvidenceRecord> records = linear_rack(0);
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
        for (const EvidenceRecord& record : records) {
            RNF_REQUIRE_OK(store->append(evidence_store_record(record)));
        }
    }
    const std::filesystem::path wal = directory / "rnf.wal";
    const std::uintmax_t full = size_of_file(wal);
    RNF_REQUIRE(full > 8);

    // Chop the last record in half: exactly what a process kill during a write
    // leaves behind.
    const std::uintmax_t chopped = full - 5;
    truncate_file(wal, chopped);
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
        RNF_CHECK(store->recovery().truncated_tail);
        RNF_CHECK(!store->recovery().corrupt_record);
        RNF_CHECK_EQ(store->recovered().records.size(), records.size() - 1);
        // Everything from the start of the incomplete record onwards is
        // discarded: its header plus the bytes that did land.
        RNF_CHECK(store->recovery().bytes_discarded > 5U);
        RNF_CHECK_EQ(store->log_bytes() + store->recovery().bytes_discarded, chopped);
        RNF_CHECK_EQ(size_of_file(wal), store->log_bytes());
        // The store is usable again and the discarded tail is really gone.
        RNF_REQUIRE_OK(store->append(evidence_store_record(records.back())));
        RNF_REQUIRE_OK(store->mark_clean_shutdown(1));
    }
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
        RNF_CHECK(!store->recovery().truncated_tail);
        RNF_CHECK_EQ(store->recovered().records.size(), records.size());
        RNF_CHECK(store->recovery().clean_shutdown);
    }
}

RNF_TEST(persist, header_only_tail_is_treated_as_torn) {
    const std::filesystem::path directory = make_temp_directory("store-header-tail");
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
        for (const EvidenceRecord& record : linear_rack(0)) {
            RNF_REQUIRE_OK(store->append(evidence_store_record(record)));
        }
    }
    const std::filesystem::path wal = directory / "rnf.wal";
    truncate_file(wal, size_of_file(wal) - 30);  // leaves a partial record header
    RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
    RNF_CHECK(store->recovery().truncated_tail);
    RNF_CHECK(store->recovered().records.size() < linear_rack(0).size());
}

RNF_TEST(persist, corrupt_record_stops_replay_at_the_prefix) {
    const std::filesystem::path directory = make_temp_directory("store-corrupt");
    const std::vector<EvidenceRecord> records = linear_rack(0);
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
        for (const EvidenceRecord& record : records) {
            RNF_REQUIRE_OK(store->append(evidence_store_record(record)));
        }
    }
    const std::filesystem::path wal = directory / "rnf.wal";
    // Corrupt a byte deep inside the first record's payload.
    flip_byte(wal, 20);
    RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
    RNF_CHECK(store->recovery().corrupt_record);
    RNF_CHECK(store->recovery().first_failure != StatusCode::kOk);
    RNF_CHECK(store->recovered().records.empty());
    RNF_CHECK(store->recovery().bytes_discarded > 0);
}

RNF_TEST(persist, checkpoint_shortens_replay_without_losing_state) {
    const std::filesystem::path directory = make_temp_directory("store-checkpoint");
    const std::vector<EvidenceRecord> records = linear_rack(0);
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
        RNF_REQUIRE_OK(store->bind_rack(kRack));
        for (const EvidenceRecord& record : records) {
            RNF_REQUIRE_OK(store->append(evidence_store_record(record)));
        }
        Checkpoint checkpoint;
        checkpoint.wal_offset = store->log_bytes();
        checkpoint.generation = TopologyGeneration{3};
        checkpoint.epoch = RackEpoch{2};
        checkpoint.incarnation = ControllerIncarnation{1};
        checkpoint.ledger = records;
        checkpoint.snapshot_bytes = {1, 2, 3, 4};
        checkpoint.snapshot_digest = Blake2s256::hash(std::string_view("snapshot"));
        checkpoint.ledger_digest = Blake2s256::hash(std::string_view("ledger"));
        RNF_REQUIRE_OK(store->write_checkpoint(checkpoint));
        // Records after the checkpoint still replay.
        RNF_REQUIRE_OK(store->append(lifecycle_store_record(LifecycleState::kActive, 3)));
        RNF_REQUIRE_OK(store->mark_clean_shutdown(2));
    }
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
        RNF_CHECK(store->recovery().checkpoint_used);
        RNF_CHECK(!store->recovery().checkpoint_rejected);
        RNF_CHECK_EQ(store->recovered().checkpoint.ledger.size(), records.size());
        RNF_CHECK_EQ(store->recovered().checkpoint.generation.value, 3U);
        RNF_CHECK_EQ(store->recovered().checkpoint.epoch.value, 2U);
        RNF_CHECK_EQ(store->recovered().checkpoint.snapshot_bytes.size(), 4U);
        RNF_CHECK_EQ(store->recovered().records.size(), 1U);
        RNF_CHECK(store->recovered().records.front().type == RecordType::kLifecycle);
    }
}

RNF_TEST(persist, corrupt_checkpoint_falls_back_to_full_replay) {
    const std::filesystem::path directory = make_temp_directory("store-bad-checkpoint");
    const std::vector<EvidenceRecord> records = linear_rack(0);
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
        for (const EvidenceRecord& record : records) {
            RNF_REQUIRE_OK(store->append(evidence_store_record(record)));
        }
        Checkpoint checkpoint;
        checkpoint.wal_offset = store->log_bytes();
        checkpoint.ledger = records;
        RNF_REQUIRE_OK(store->write_checkpoint(checkpoint));
    }
    flip_byte(directory / "rnf.checkpoint", 40);
    RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
    RNF_CHECK(store->recovery().checkpoint_rejected);
    RNF_CHECK(!store->recovery().checkpoint_used);
    // The log still holds every record, so nothing is lost.
    RNF_CHECK_EQ(store->recovered().records.size(), records.size());
}

RNF_TEST(persist, incompatible_format_version_is_refused) {
    const std::filesystem::path directory = make_temp_directory("store-version");
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, default_options()));
        RNF_REQUIRE(store->next_incarnation().has_value());
        RNF_REQUIRE_OK(store->mark_clean_shutdown(1));
    }
    // Byte 8 of the manifest is the little-endian format version.
    std::vector<char> bytes;
    {
        std::ifstream input(directory / "rnf.meta", std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    RNF_REQUIRE(bytes.size() >= 12);
    bytes[8] = static_cast<char>(kStateFormatVersion + 1);
    {
        std::ofstream output(directory / "rnf.meta", std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    // The version field is covered by the manifest checksum, so an edited
    // version without a repaired checksum is an integrity failure.
    RNF_REQUIRE_CODE(Store::open(directory, default_options()), StatusCode::kIntegrityFailure);

    // With the checksum repaired, the manifest is readable and the version is
    // what is wrong, so the refusal names the version.
    std::vector<std::uint8_t> raw(bytes.begin(), bytes.end());
    raw[12] = 0;
    raw[13] = 0;
    raw[14] = 0;
    raw[15] = 0;
    const std::uint32_t crc = crc32c(ByteSpan(raw.data(), raw.size()));
    bytes[12] = static_cast<char>(crc & 0xFFU);
    bytes[13] = static_cast<char>((crc >> 8U) & 0xFFU);
    bytes[14] = static_cast<char>((crc >> 16U) & 0xFFU);
    bytes[15] = static_cast<char>((crc >> 24U) & 0xFFU);
    {
        std::ofstream output(directory / "rnf.meta", std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    RNF_REQUIRE_CODE(Store::open(directory, default_options()), StatusCode::kIncompatibleVersion);
}

RNF_TEST(persist, a_second_open_of_the_same_directory_is_refused) {
    const std::filesystem::path directory = make_temp_directory("store-lock");
    RNF_REQUIRE_VALUE(first, Store::open(directory, default_options()));
    RNF_REQUIRE_CODE(Store::open(directory, default_options()), StatusCode::kBusy);
    // Releasing the first store releases the lock, and a hard process exit
    // releases it too because the operating system owns the handle.
    first.reset();
    RNF_REQUIRE_VALUE(second, Store::open(directory, default_options()));
    RNF_REQUIRE_OK(second->mark_clean_shutdown(1));
}

RNF_TEST(persist, record_encoding_roundtrips_and_refuses_truncation) {
    const std::vector<StoreRecord> samples = {
        evidence_store_record(device_record(1, DeviceRole::kSwitch, Capacity{5})),
        lifecycle_store_record(LifecycleState::kDraining, 9),
        [] {
            StoreRecord record;
            record.type = RecordType::kEpoch;
            EpochRecord body;
            body.epoch = RackEpoch{4};
            body.member_set_digest = Blake2s256::hash(std::string_view("members"));
            record.payload = body;
            return record;
        }(),
        [] {
            StoreRecord record;
            record.type = RecordType::kGrantState;
            GrantStateRecord body;
            body.id = GrantId{3};
            body.state = GrantState::kReleased;
            body.reason = "released by holder";
            record.payload = std::move(body);
            return record;
        }(),
        [] {
            StoreRecord record;
            record.type = RecordType::kCleanShutdown;
            ShutdownRecord body;
            body.at_ms = 42;
            record.payload = body;
            return record;
        }(),
    };
    for (const StoreRecord& sample : samples) {
        ByteWriter writer(4096);
        RNF_REQUIRE_OK(encode(sample, writer));
        ByteReader reader(writer.span());
        RNF_REQUIRE_VALUE(decoded, decode_record(reader));
        RNF_CHECK(decoded == sample);
        RNF_CHECK(reader.at_end());
        for (std::size_t cut = 0; cut < writer.size(); ++cut) {
            ByteReader short_reader(ByteSpan(writer.data().data(), cut));
            RNF_CHECK(!decode_record(short_reader).has_value());
        }
    }
    // An unknown record type is refused rather than skipped silently.
    const std::uint8_t unknown[1] = {0x7F};
    ByteReader reader(ByteSpan(unknown, 1));
    RNF_REQUIRE_CODE(decode_record(reader).error(), StatusCode::kMalformedEncoding);
}

RNF_TEST(persist, bounded_record_size_is_enforced) {
    StoreOptions options = default_options();
    options.max_record_bytes = 64;
    const std::filesystem::path directory = make_temp_directory("store-bounds");
    RNF_REQUIRE_VALUE(store, Store::open(directory, options));
    StoreRecord record = evidence_store_record(
        attachment_record(1, 1, 1, std::string(200, 'h')));
    RNF_REQUIRE_CODE(store->append(record), StatusCode::kOversizeField);
    RNF_CHECK_EQ(store->appends(), 0U);
    RNF_REQUIRE_OK(store->mark_clean_shutdown(1));
}
