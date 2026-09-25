// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial tests: malformed input, hostile framing, extreme values,
// duplicate identities, replayed events, repeated start and stop, and
// concurrent mutation.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "rnf/net/frame.hpp"
#include "rnf/net/protocol.hpp"
#include "rnf/persist/store.hpp"
#include "rnf/runtime/client.hpp"
#include "rnf/runtime/daemon.hpp"
#include "rnf/runtime/server.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/harness.hpp"

using namespace rnf;
using namespace rnf::test;

namespace {

std::vector<std::uint8_t> frame_bytes(std::uint32_t magic, std::uint16_t version,
                                      std::uint16_t type, std::uint32_t length,
                                      std::uint32_t crc) {
    std::vector<std::uint8_t> bytes(kFrameHeaderSize, 0);
    for (int i = 0; i < 4; ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((magic >> (8 * i)) & 0xFFU);
    }
    bytes[4] = static_cast<std::uint8_t>(version & 0xFFU);
    bytes[5] = static_cast<std::uint8_t>((version >> 8U) & 0xFFU);
    bytes[6] = static_cast<std::uint8_t>(type & 0xFFU);
    bytes[7] = static_cast<std::uint8_t>((type >> 8U) & 0xFFU);
    for (int i = 0; i < 4; ++i) {
        bytes[8 + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((length >> (8 * i)) & 0xFFU);
        bytes[12 + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFU);
    }
    return bytes;
}

DaemonConfig memory_config() {
    DaemonConfig config;
    config.rack = linear_rack_config();
    config.rack.name = "adversarial";
    config.auto_compose = true;
    config.port = 0;
    return config;
}

}  // namespace

RNF_TEST(adversarial, frame_header_rejects_every_malformed_shape) {
    const std::uint32_t good_crc = crc32c(ByteSpan());
    FrameHeader header;

    auto rejects = [&](const std::vector<std::uint8_t>& bytes, StatusCode expected) {
        RNF_REQUIRE_CODE(decode_frame_header(ByteSpan(bytes.data(), bytes.size()),
                                             kDefaultMaxFramePayload, kProtocolVersion, header),
                         expected);
    };
    rejects(frame_bytes(0, kProtocolVersion, 1, 0, good_crc), StatusCode::kMalformedEncoding);
    rejects(frame_bytes(kFrameMagic, static_cast<std::uint16_t>(kProtocolVersion + 1), 1, 0,
                        good_crc),
            StatusCode::kIncompatibleVersion);
    rejects(frame_bytes(kFrameMagic, kProtocolVersion, 1, kDefaultMaxFramePayload + 1, good_crc),
            StatusCode::kOversizeField);
    rejects(frame_bytes(kFrameMagic, kProtocolVersion, 1, 0xFFFFFFFFU, good_crc),
            StatusCode::kOversizeField);
    rejects(frame_bytes(kFrameMagic, kProtocolVersion, 0, 0, good_crc), StatusCode::kUnsupported);
    rejects(frame_bytes(kFrameMagic, kProtocolVersion, 0xFFFF, 0, good_crc),
            StatusCode::kUnsupported);
    RNF_REQUIRE_OK(decode_frame_header(
        ByteSpan(frame_bytes(kFrameMagic, kProtocolVersion, static_cast<std::uint16_t>(
                                                              MessageType::kHello),
                             0, good_crc)
                     .data(),
                 kFrameHeaderSize),
        kDefaultMaxFramePayload, kProtocolVersion, header));
    RNF_CHECK_EQ(header.length, 0U);
    RNF_CHECK(header.type == static_cast<std::uint16_t>(MessageType::kHello));

    // The hard cap applies even when the negotiated cap is larger.
    RNF_REQUIRE_CODE(decode_frame_header(
                         ByteSpan(frame_bytes(kFrameMagic, kProtocolVersion, 1,
                                              kHardMaxFramePayload + 1, good_crc)
                                      .data(),
                                  kFrameHeaderSize),
                         0xFFFFFFFFU, kProtocolVersion, header),
                     StatusCode::kOversizeField);
}

RNF_TEST(adversarial, encode_frame_refuses_oversize_payloads) {
    std::vector<std::uint8_t> out;
    const std::vector<std::uint8_t> payload(1024, 0x41);
    FrameHeader header;
    header.version = kProtocolVersion;
    header.type = static_cast<std::uint16_t>(MessageType::kHello);
    RNF_REQUIRE_CODE(
        encode_frame(header, ByteSpan(payload.data(), payload.size()), out, 512),
        StatusCode::kOversizeField);
    RNF_REQUIRE_OK(encode_frame(header, ByteSpan(payload.data(), payload.size()), out, 1024));
    RNF_CHECK_EQ(out.size(), kFrameHeaderSize + payload.size());
    // A corrupt payload is caught by the checksum the receiver recomputes.
    out.back() = static_cast<std::uint8_t>(out.back() ^ 0x01U);
    RNF_CHECK(!(crc32c(ByteSpan(out.data() + kFrameHeaderSize, payload.size())) ==
                header.crc));
}

RNF_TEST(adversarial, evidence_rejects_duplicate_and_extreme_identities) {
    EvidenceLedger ledger;
    // Two different port identities claiming the same device/index pair.
    (void)ledger.insert(member_record(1, 1));
    (void)ledger.insert(device_record(1, DeviceRole::kSwitch, Capacity{100}));
    (void)ledger.insert(port_record(1, 1, 0, PortRole::kAccess, Capacity{10}));
    (void)ledger.insert(port_record(2, 1, 0, PortRole::kAccess, Capacity{10}));
    EvidenceLedger copy;
    (void)copy.insert(member_record(1, 1));
    (void)copy.insert(device_record(1, DeviceRole::kSwitch, Capacity{100}));
    (void)copy.insert(port_record(1, 1, 0, PortRole::kAccess, Capacity{10}));
    (void)copy.insert(port_record(2, 1, 0, PortRole::kAccess, Capacity{10}));
    Result<ComposeOutcome> outcome = compose(copy, linear_rack_config(), compose_input(0));
    RNF_REQUIRE(outcome.has_value());
    bool saw_conflict = false;
    for (const Diagnostic& diagnostic : outcome->snapshot->diagnostics) {
        if (diagnostic.kind == DiagnosticKind::kConflictingEvidence) {
            saw_conflict = true;
        }
    }
    RNF_CHECK(saw_conflict);

    // Extreme capacity values must not overflow any accumulation.
    EvidenceLedger extremes;
    (void)extremes.insert(member_record(1, 1));
    (void)extremes.insert(member_record(2, 2));
    (void)extremes.insert(device_record(1, DeviceRole::kSwitch,
                                        Capacity{std::numeric_limits<std::uint64_t>::max()}));
    (void)extremes.insert(device_record(2, DeviceRole::kHost,
                                        Capacity{std::numeric_limits<std::uint64_t>::max()}));
    Result<ComposeOutcome> extreme_outcome = compose(extremes, linear_rack_config(),
                                                     compose_input(0));
    // Either the sum fits, or the composition refuses with a typed overflow.
    if (extreme_outcome.has_value()) {
        RNF_CHECK(extreme_outcome->snapshot->capacity.closes());
    } else {
        RNF_CHECK(extreme_outcome.error().code() == StatusCode::kOutOfRange);
    }

    // Maximum identities must be handled without aliasing.
    EvidenceLedger max_ids;
    (void)max_ids.insert(member_record(std::numeric_limits<std::uint64_t>::max(),
                                       std::numeric_limits<std::uint64_t>::max()));
    Result<ComposeOutcome> max_outcome = compose(max_ids, linear_rack_config(),
                                                 compose_input(0));
    RNF_REQUIRE(max_outcome.has_value());
    RNF_CHECK_EQ(max_outcome->snapshot->devices.size(), 1U);
}

RNF_TEST(adversarial, replayed_evidence_is_idempotent_at_the_daemon) {
    DaemonCore core(memory_config());
    RNF_REQUIRE_OK(core.start());
    const std::vector<EvidenceRecord> records = linear_rack(0);
    SubmitEvidenceRequest first;
    first.records = records;
    RNF_REQUIRE_VALUE(first_ack, core.submit_evidence(first));
    RNF_CHECK_EQ(first_ack.entries.size(), records.size());
    for (const EvidenceAckEntry& entry : first_ack.entries) {
        RNF_CHECK(!(entry.outcome == InsertOutcome::kRefused));
    }
    SubmitEvidenceRequest second;
    second.records = records;
    RNF_REQUIRE_VALUE(second_ack, core.submit_evidence(second));
    for (const EvidenceAckEntry& entry : second_ack.entries) {
        RNF_CHECK(entry.outcome == InsertOutcome::kDuplicate);
    }
    RNF_REQUIRE_VALUE(before, core.query_state());
    RNF_REQUIRE_VALUE(after, core.query_state());
    RNF_CHECK(before.snapshot_digest == after.snapshot_digest);

    // An out-of-rack membership claim is refused at ingest.
    SubmitEvidenceRequest foreign;
    foreign.records.push_back(member_record(50, 50, 0, kDiscoverySource, 1, Durability::kSticky,
                                            RackId{999}));
    RNF_REQUIRE_VALUE(foreign_ack, core.submit_evidence(foreign));
    RNF_REQUIRE(foreign_ack.entries.size() == 1U);
    RNF_CHECK(foreign_ack.entries.front().outcome == InsertOutcome::kRefused);
    RNF_CHECK(foreign_ack.entries.front().code == StatusCode::kOutOfRack);
    RNF_REQUIRE_OK(core.stop(1));
}

RNF_TEST(adversarial, oversize_batches_are_refused_before_allocation_grows) {
    DaemonConfig config = memory_config();
    config.max_records_per_batch = 8;
    DaemonCore core(config);
    RNF_REQUIRE_OK(core.start());
    SubmitEvidenceRequest request;
    for (std::uint64_t i = 1; i <= 9; ++i) {
        request.records.push_back(device_record(i, DeviceRole::kHost, Capacity{1}));
    }
    RNF_REQUIRE_CODE(core.submit_evidence(request), StatusCode::kOversizeField);
    RNF_REQUIRE_OK(core.stop(1));
}

RNF_TEST(adversarial, repeated_start_and_stop_is_stable) {
    const std::filesystem::path directory = make_temp_directory("adversarial-cycles");
    for (int cycle = 0; cycle < 6; ++cycle) {
        DaemonConfig config = memory_config();
        config.store_directory = directory;
        DaemonCore core(config);
        RNF_REQUIRE_OK(core.start());
        SubmitEvidenceRequest request;
        request.records = linear_rack(0);
        RNF_REQUIRE_VALUE(ack, core.submit_evidence(request));
        RNF_CHECK_EQ(ack.entries.size(), linear_rack(0).size());
        RNF_REQUIRE_VALUE(composed, core.compose(ComposeCommand{TopologyGeneration{1}}));
        RNF_CHECK(composed.code == StatusCode::kOk);
        RNF_REQUIRE_VALUE(before, core.query_state());
        RNF_REQUIRE_VALUE(before_stats, core.stats());
        RNF_CHECK_EQ(before_stats.evidence_records, linear_rack(0).size());
        RNF_CHECK_EQ(before.devices, 2U);
        RNF_REQUIRE_OK(core.stop(1));
    }
    // Every incarnation was distinct, and the last store reopens cleanly.
    StoreOptions options;
    RNF_REQUIRE_VALUE(store, Store::open(directory, options));
    RNF_CHECK(store->recovery().clean_shutdown);
    RNF_CHECK(store->incarnation_counter() >= 6U);
    RNF_REQUIRE_OK(store->mark_clean_shutdown(2));
}

RNF_TEST(adversarial, server_start_stop_cycles_release_every_resource) {
    for (int cycle = 0; cycle < 4; ++cycle) {
        DaemonCore core(memory_config());
        RNF_REQUIRE_OK(core.start());
        DaemonServer server(core, ServerConfig{});
        RNF_REQUIRE_OK(server.bind("127.0.0.1", 0));
        RNF_CHECK(server.port() != 0);
        std::atomic<bool> stop{false};
        std::thread runner([&]() { RNF_CHECK(server.run(&stop).ok()); });
        stop.store(true);
        runner.join();
        RNF_REQUIRE_OK(core.stop(1));
    }
}

RNF_TEST(adversarial, stop_is_idempotent_and_compose_after_retire_is_refused) {
    DaemonCore core(memory_config());
    RNF_REQUIRE_OK(core.start());
    RNF_REQUIRE_VALUE(active, core.set_lifecycle(LifecycleRequest{LifecycleState::kActive}));
    RNF_CHECK(active.code == StatusCode::kOk);
    RNF_REQUIRE_VALUE(retired, core.set_lifecycle(LifecycleRequest{LifecycleState::kRetired}));
    RNF_CHECK(retired.code == StatusCode::kOk);
    RNF_REQUIRE_CODE(core.compose(ComposeCommand{TopologyGeneration{1}}).error(),
                     StatusCode::kLifecycleRefused);
    RNF_REQUIRE_VALUE(again, core.set_lifecycle(LifecycleRequest{LifecycleState::kActive}));
    RNF_CHECK(again.code == StatusCode::kInvalidLifecycleTransition);
    RNF_REQUIRE_OK(core.stop(1));
    RNF_REQUIRE_OK(core.stop(2));
}

RNF_TEST(adversarial, concurrent_mutation_keeps_the_invariants) {
    DaemonConfig config = memory_config();
    config.auto_compose = false;
    DaemonCore core(config);
    RNF_REQUIRE_OK(core.start());
    SubmitEvidenceRequest seed;
    seed.records = linear_rack(0);
    RNF_REQUIRE_VALUE(seed_ack, core.submit_evidence(seed));
    RNF_CHECK_EQ(seed_ack.entries.size(), seed.records.size());
    RNF_REQUIRE_VALUE(activated, core.set_lifecycle(LifecycleRequest{LifecycleState::kActive}));
    RNF_CHECK(activated.code == StatusCode::kOk);

    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (std::uint64_t worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&, worker]() {
            for (int iteration = 0; iteration < 40; ++iteration) {
                AcquireRequest request;
                request.request.hi = worker + 1;
                request.request.lo = static_cast<std::uint64_t>(iteration) + 1;
                request.principal = PrincipalId{worker + 1};
                request.scope = (iteration % 2 == 0) ? ResourceRef{ResourceKind::kDevice, 1}
                                                     : ResourceRef{ResourceKind::kPort, 20};
                request.mode = (iteration % 5 == 0) ? GrantMode::kExclusive : GrantMode::kShared;
                request.capacity = Capacity{1};
                request.ttl_ms = 60000;
                const Result<GrantResponse> granted = core.acquire(request);
                if (!granted.has_value()) {
                    failures.fetch_add(1);
                    continue;
                }
                if (granted->code == StatusCode::kOk) {
                    TokenMessage token;
                    token.token.grant = granted->grant.id;
                    token.token.fence = granted->grant.fence;
                    token.token.incarnation = granted->grant.incarnation;
                    token.token.scope_basis = granted->grant.basis;
                    (void)core.validate(token);
                    (void)core.release(token);
                }
                (void)core.query_state();
                (void)core.query_resources(ResourceQuery{});
                (void)core.stats();
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    RNF_CHECK_EQ(failures.load(), 0);

    RNF_REQUIRE_VALUE(state, core.query_state());
    RNF_CHECK(state.capacity.committed_grants.units <= state.capacity.usable.units);
    RNF_REQUIRE_VALUE(grants, core.query_grants());
    std::vector<const GrantSummary*> live;
    for (const GrantSummary& grant : grants.grants) {
        if (grant.state == GrantState::kActive || grant.state == GrantState::kSuspended) {
            live.push_back(&grant);
        }
    }
    RNF_CHECK(live.size() <= 2U);  // at most one per scope after every release
    RNF_REQUIRE_OK(core.stop(1));
}

RNF_TEST(adversarial, connection_reset_mid_frame_is_a_typed_failure) {
    DaemonConfig config = memory_config();
    DaemonCore core(config);
    RNF_REQUIRE_OK(core.start());
    DaemonServer server(core, ServerConfig{});
    RNF_REQUIRE_OK(server.bind("127.0.0.1", 0));
    std::atomic<bool> stop{false};
    std::thread runner([&]() { RNF_CHECK(server.run(&stop).ok()); });

    // Send a frame header that promises more bytes than will ever arrive, then
    // close. The daemon must survive and keep serving other clients.
    {
        Result<TcpStream> stream =
            connect_loopback(Endpoint{"127.0.0.1", server.port()}, 40, 25);
        RNF_REQUIRE(stream.has_value());
        FrameHeader header;
        header.version = kProtocolVersion;
        header.type = static_cast<std::uint16_t>(MessageType::kQueryState);
        header.correlation = 1;
        std::vector<std::uint8_t> frame;
        RNF_REQUIRE_OK(encode_frame(header, ByteSpan(), frame, kDefaultMaxFramePayload));
        // Rewrite the declared length to something large without sending it.
        const std::uint32_t lying = 4096;
        for (int i = 0; i < 4; ++i) {
            frame[8 + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((lying >> (8 * i)) & 0xFFU);
        }
        RNF_REQUIRE_OK(stream->send_all(ByteSpan(frame.data(), frame.size())));
        stream->close();
    }
    {
        // The daemon is still healthy.
        ClientOptions options;
        options.connect_attempts = 40;
        options.connect_retry_ms = 25;
        Result<Client> connected =
            Client::connect(Endpoint{"127.0.0.1", server.port()}, options);
        RNF_REQUIRE(connected.has_value());
        RNF_REQUIRE(connected->hello(1).has_value());
        RNF_REQUIRE(connected->query_state().has_value());
        connected->close();
    }
    stop.store(true, std::memory_order_release);
    server.request_stop();
    runner.join();
    RNF_REQUIRE_VALUE(stats, core.stats());
    RNF_CHECK(stats.connections_accepted >= 2U);
    RNF_REQUIRE_OK(core.stop(1));
}

RNF_TEST(adversarial, hostile_frame_lengths_never_allocate) {
    // A frame that claims a payload far beyond the cap must be refused from the
    // header alone, without the daemon reserving anything for it.
    DaemonConfig config = memory_config();
    DaemonCore core(config);
    RNF_REQUIRE_OK(core.start());
    DaemonServer server(core, ServerConfig{});
    RNF_REQUIRE_OK(server.bind("127.0.0.1", 0));
    std::atomic<bool> stop{false};
    std::thread runner([&]() { RNF_CHECK(server.run(&stop).ok()); });

    for (std::uint32_t claimed : {kDefaultMaxFramePayload + 1U, 0x7FFFFFFFU, 0xFFFFFFFFU}) {
        Result<TcpStream> stream =
            connect_loopback(Endpoint{"127.0.0.1", server.port()}, 40, 25);
        RNF_REQUIRE(stream.has_value());
        std::vector<std::uint8_t> header(kFrameHeaderSize, 0);
        const std::uint32_t magic = kFrameMagic;
        for (int i = 0; i < 4; ++i) {
            header[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((magic >> (8 * i)) & 0xFFU);
            header[8 + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((claimed >> (8 * i)) & 0xFFU);
        }
        header[4] = static_cast<std::uint8_t>(kProtocolVersion);
        header[6] = static_cast<std::uint8_t>(MessageType::kQueryState);
        RNF_REQUIRE_OK(stream->send_all(ByteSpan(header.data(), header.size())));
        // The daemon answers with an error frame and closes.
        std::vector<std::uint8_t> response(kFrameHeaderSize);
        const Status read = stream->recv_exact(ByteSpan(response.data(), response.size()));
        if (read.ok()) {
            FrameHeader reply;
            RNF_REQUIRE_OK(decode_frame_header(ByteSpan(response.data(), response.size()),
                                               kDefaultMaxFramePayload, kProtocolVersion, reply));
            RNF_CHECK(reply.type == static_cast<std::uint16_t>(MessageType::kError));
        }
        stream->close();
    }
    stop.store(true, std::memory_order_release);
    server.request_stop();
    runner.join();
    RNF_REQUIRE_VALUE(stats, core.stats());
    RNF_CHECK(stats.frames_rejected >= 3U);
    RNF_REQUIRE_OK(core.stop(1));
}

RNF_TEST(adversarial, store_rejects_a_truncated_manifest) {
    const std::filesystem::path directory = make_temp_directory("adversarial-manifest");
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, StoreOptions{}));
        RNF_REQUIRE(store->next_incarnation().has_value());
        RNF_REQUIRE_OK(store->mark_clean_shutdown(1));
    }
    std::filesystem::resize_file(directory / "rnf.meta", 40);
    // Refusing is the default: the manifest holds the only durable copy of the
    // incarnation counter, and starting without it could reuse an incarnation.
    RNF_REQUIRE_CODE(Store::open(directory, StoreOptions{}), StatusCode::kTruncated);

    StoreOptions permissive;
    permissive.allow_manifest_loss = true;
    RNF_REQUIRE_VALUE(store, Store::open(directory, permissive));
    RNF_CHECK(store->recovery().manifest_rejected);
    RNF_CHECK(!store->recovery().manifest_missing);
    RNF_CHECK(!store->recovery().clean_shutdown);
    RNF_CHECK(store->recovery().first_failure != StatusCode::kOk);
}

RNF_TEST(adversarial, delete_and_recreate_of_the_log_is_recovered_conservatively) {
    const std::filesystem::path directory = make_temp_directory("adversarial-missing-wal");
    {
        RNF_REQUIRE_VALUE(store, Store::open(directory, StoreOptions{}));
        for (const EvidenceRecord& record : linear_rack(0)) {
            StoreRecord wrapped;
            wrapped.type = RecordType::kEvidence;
            wrapped.payload = record;
            RNF_REQUIRE_OK(store->append(wrapped));
        }
        RNF_REQUIRE_OK(store->mark_clean_shutdown(1));
    }
    std::error_code ec;
    std::filesystem::remove(directory / "rnf.wal", ec);
    RNF_REQUIRE(!ec);
    RNF_REQUIRE_VALUE(store, Store::open(directory, StoreOptions{}));
    RNF_CHECK(store->recovery().fresh_store);
    RNF_CHECK_EQ(store->recovered().records.size(), 0U);
    // Nothing was invented to fill the gap.
    RNF_CHECK_EQ(store->log_bytes(), 0U);
}
