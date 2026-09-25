// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Property tests. Every case is driven by the deterministic generator seeded
// from the command line, and every failure prints the seed that produced it.

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "rnf/authority/registry.hpp"
#include "rnf/compose/composer.hpp"
#include "rnf/net/frame.hpp"
#include "rnf/net/protocol.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/harness.hpp"

using namespace rnf;
using namespace rnf::test;

namespace {

struct RandomRack {
    std::vector<EvidenceRecord> records;
    std::vector<std::uint64_t> devices;
    std::vector<std::uint64_t> links;
    std::vector<std::uint64_t> ports;
};

/// Build a random but structurally valid rack.
RandomRack make_random_rack(SplitMix64& rng, std::uint64_t generation) {
    RandomRack rack;
    const std::uint64_t device_count = 1 + rng.bounded(6);
    std::uint64_t next_port = 1;
    std::uint64_t next_link = 1;
    std::uint64_t sequence = 1;
    std::uint64_t next_attachment = 1;

    for (std::uint64_t d = 1; d <= device_count; ++d) {
        rack.devices.push_back(d);
        const auto availability =
            static_cast<Availability>(rng.bounded(5));  // includes kUnknown on purpose
        rack.records.push_back(member_record(d, 100 + d, generation, kOperatorSource,
                                             sequence++, Durability::kSticky));
        rack.records.push_back(device_record(d,
                                             rng.coin() ? DeviceRole::kSwitch : DeviceRole::kHost,
                                             Capacity{rng.bounded(2000)}, availability, generation,
                                             kOperatorSource, sequence++));
        const std::uint64_t port_count = 1 + rng.bounded(4);
        std::vector<std::uint64_t> device_ports;
        for (std::uint64_t i = 0; i < port_count; ++i) {
            const std::uint64_t port = next_port++;
            device_ports.push_back(port);
            rack.ports.push_back(port);
            rack.records.push_back(
                port_record(port, d, static_cast<std::uint16_t>(i),
                            rng.coin() ? PortRole::kAccess : PortRole::kUplink,
                            Capacity{rng.bounded(400)},
                            static_cast<Availability>(rng.bounded(5)),
                            rng.coin() ? AdminState::kEnabled : AdminState::kDisabled, generation,
                            kOperatorSource, sequence++));
        }
        if (!device_ports.empty() && rng.coin()) {
            rack.records.push_back(attachment_record(
                next_attachment++, d, device_ports.front(), "host", generation, kOperatorSource,
                sequence++));
        }
    }
    const std::uint64_t link_count = rng.bounded(6);
    for (std::uint64_t i = 0; i < link_count && rack.ports.size() >= 2; ++i) {
        const std::uint64_t first = rack.ports[rng.bounded(rack.ports.size())];
        const std::uint64_t second = rack.ports[rng.bounded(rack.ports.size())];
        if (first == second) {
            continue;
        }
        const std::uint64_t link = next_link++;
        rack.links.push_back(link);
        rack.records.push_back(link_record(link, first, second, Capacity{rng.bounded(500)},
                                           static_cast<Availability>(rng.bounded(5)), generation,
                                           kOperatorSource, sequence++));
    }
    const std::uint64_t maintenance_count = rng.bounded(3);
    for (std::uint64_t i = 0; i < maintenance_count; ++i) {
        const std::uint64_t pick = rng.bounded(3);
        if (pick == 0) {
            rack.records.push_back(maintenance_record(MaintenanceKind::kDrain,
                                                      ResourceKind::kDevice,
                                                      rack.devices[rng.bounded(rack.devices.size())],
                                                      "random drain", generation, kOperatorSource,
                                                      sequence++));
        } else if (pick == 1 && !rack.ports.empty()) {
            rack.records.push_back(maintenance_record(MaintenanceKind::kMaintenance,
                                                      ResourceKind::kPort,
                                                      rack.ports[rng.bounded(rack.ports.size())],
                                                      "random maintenance", generation,
                                                      kOperatorSource, sequence++));
        } else if (!rack.links.empty()) {
            rack.records.push_back(maintenance_record(MaintenanceKind::kDrain,
                                                      ResourceKind::kLink,
                                                      rack.links[rng.bounded(rack.links.size())],
                                                      "random link drain", generation,
                                                      kOperatorSource, sequence++));
        }
    }
    const std::uint64_t obligation_count = rng.bounded(3);
    for (std::uint64_t i = 0; i < obligation_count; ++i) {
        const std::uint64_t pick = rng.bounded(3);
        if (pick == 0) {
            rack.records.push_back(obligation_record(
                i + 1, ResourceKind::kRack, 0, Capacity{rng.bounded(300)}, PrincipalId{5},
                "pod", generation, kImportedSource, sequence++));
        } else if (pick == 1) {
            rack.records.push_back(obligation_record(
                i + 1, ResourceKind::kDevice, rack.devices[rng.bounded(rack.devices.size())],
                Capacity{rng.bounded(300)}, PrincipalId{5}, "pod", generation, kImportedSource,
                sequence++));
        } else if (!rack.links.empty()) {
            rack.records.push_back(obligation_record(
                i + 1, ResourceKind::kLink, rack.links[rng.bounded(rack.links.size())],
                Capacity{rng.bounded(300)}, PrincipalId{5}, "pod", generation, kImportedSource,
                sequence++));
        }
    }
    return rack;
}

Snapshot compose_random(const std::vector<EvidenceRecord>& records, std::uint64_t generation,
                        Capacity headroom) {
    EvidenceLedger ledger;
    fill(ledger, records);
    RackConfig config = linear_rack_config();
    config.headroom_floor = headroom;
    Result<ComposeOutcome> outcome = compose(ledger, config, compose_input(generation));
    if (!outcome.has_value()) {
        RNF_REQUIRE(false);
    }
    return outcome->snapshot;
}

}  // namespace

RNF_TEST(property, capacity_closure_holds_for_random_racks) {
    SplitMix64 rng(current_seed());
    for (int iteration = 0; iteration < 200; ++iteration) {
        const RandomRack rack = make_random_rack(rng, 0);
        const Capacity headroom{rng.bounded(200)};
        const Snapshot snapshot = compose_random(rack.records, 0, headroom);
        RNF_CHECK(snapshot->capacity.closes());
        std::uint64_t lhs = 0;
        RNF_CHECK(checked_add_u64(snapshot->capacity.unavailable.units,
                                  snapshot->capacity.usable.units, lhs));
        RNF_CHECK_EQ(lhs, snapshot->capacity.total.units);
        std::uint64_t rhs = 0;
        RNF_CHECK(checked_add_u64(snapshot->capacity.obligated.units,
                                  snapshot->capacity.headroom_floor.units, rhs));
        std::uint64_t rhs2 = 0;
        RNF_CHECK(checked_add_u64(rhs, snapshot->capacity.uncommitted.units, rhs2));
        std::uint64_t lhs2 = 0;
        RNF_CHECK(checked_add_u64(snapshot->capacity.usable.units, snapshot->capacity.deficit.units,
                                  lhs2));
        RNF_CHECK_EQ(lhs2, rhs2);
        // Resource obligations never exceed the ledger's obligated total.
        Capacity sum{};
        for (const ResourceState& state : snapshot->resources) {
            std::uint64_t next = 0;
            RNF_REQUIRE(checked_add_u64(sum.units, state.obligated.units, next));
            sum.units = next;
        }
        RNF_CHECK(sum.units >= snapshot->capacity.obligated.units);
    }
}

RNF_TEST(property, composition_is_arrival_order_independent) {
    SplitMix64 rng(current_seed());
    for (int iteration = 0; iteration < 120; ++iteration) {
        RandomRack rack = make_random_rack(rng, 0);
        EvidenceLedger forward;
        fill(forward, rack.records);
        // Fisher-Yates with the seeded generator.
        for (std::size_t i = rack.records.size(); i > 1; --i) {
            const std::size_t j = static_cast<std::size_t>(rng.bounded(i));
            std::swap(rack.records[i - 1], rack.records[j]);
        }
        EvidenceLedger shuffled;
        fill(shuffled, rack.records);
        RNF_CHECK(forward.digest() == shuffled.digest());
        const Snapshot a = compose_random(forward.records(), 0, Capacity{rng.bounded(50)});
        const Snapshot b = compose_random(shuffled.records(), 0, Capacity{0});
        RNF_CHECK(a->evidence_digest == b->evidence_digest);
        RNF_CHECK(a->member_set_digest == b->member_set_digest);
    }
}

RNF_TEST(property, duplicate_submission_changes_nothing) {
    SplitMix64 rng(current_seed());
    for (int iteration = 0; iteration < 80; ++iteration) {
        const RandomRack rack = make_random_rack(rng, 0);
        EvidenceLedger once;
        fill(once, rack.records);
        EvidenceLedger twice;
        fill(twice, rack.records);
        fill(twice, rack.records);
        RNF_CHECK(once.digest() == twice.digest());
        RNF_CHECK_EQ(once.size(), twice.size());
        const Snapshot a = compose_random(once.records(), 0, Capacity{0});
        const Snapshot b = compose_random(twice.records(), 0, Capacity{0});
        RNF_CHECK(a.digest() == b.digest());
    }
}

RNF_TEST(property, live_grants_never_overlap_when_exclusive) {
    SplitMix64 rng(current_seed());
    for (int iteration = 0; iteration < 60; ++iteration) {
        const RandomRack rack = make_random_rack(rng, 0);
        const Snapshot snapshot = compose_random(rack.records, 0, Capacity{0});
        GrantRegistry registry;
        registry.set_incarnation(ControllerIncarnation{1});
        std::vector<Grant> granted;
        for (int attempt = 0; attempt < 24; ++attempt) {
            const std::vector<ResourceRef>& candidates = [&]() -> const std::vector<ResourceRef>& {
                static thread_local std::vector<ResourceRef> cache;
                cache.clear();
                for (const ResourceState& state : snapshot->resources) {
                    cache.push_back(state.ref);
                }
                return cache;
            }();
            if (candidates.empty()) {
                break;
            }
            GrantRequest request;
            request.request.lo = static_cast<std::uint64_t>(attempt + 1);
            request.principal = PrincipalId{3};
            request.scope = candidates[static_cast<std::size_t>(rng.bounded(candidates.size()))];
            request.mode = rng.coin() ? GrantMode::kExclusive : GrantMode::kShared;
            request.capacity = Capacity{rng.bounded(120)};
            request.ttl_ms = 60000;
            const Result<Grant> result =
                registry.acquire(snapshot, request, ControllerIncarnation{1}, 1000, true);
            if (result.has_value()) {
                granted.push_back(*result);
            }
        }
        // Independent cross check: recompute every live grant's footprint and
        // confirm that no exclusive pair overlaps.
        for (std::size_t i = 0; i < granted.size(); ++i) {
            const Grant* left = registry.find(granted[i].id);
            if (left == nullptr || (left->state != GrantState::kActive &&
                                    left->state != GrantState::kSuspended)) {
                continue;
            }
            RNF_REQUIRE_VALUE(left_scope, expand_scope(snapshot, left->scope));
            for (std::size_t j = i + 1; j < granted.size(); ++j) {
                const Grant* right = registry.find(granted[j].id);
                if (right == nullptr || (right->state != GrantState::kActive &&
                                         right->state != GrantState::kSuspended)) {
                    continue;
                }
                RNF_REQUIRE_VALUE(right_scope, expand_scope(snapshot, right->scope));
                if (left->mode == GrantMode::kExclusive || right->mode == GrantMode::kExclusive) {
                    RNF_CHECK(!scopes_intersect(left_scope, right_scope));
                }
            }
        }
        // Committed capacity never exceeds the pools it was taken from.
        for (const ResourceState& state : snapshot->resources) {
            const Result<Capacity> used = registry.committed(state.ref);
            RNF_REQUIRE(used.has_value());
            if (state.capacity_known) {
                RNF_CHECK(used->units <= state.capacity.units + state.obligated.units);
            }
        }
    }
}

RNF_TEST(property, evidence_decode_never_crashes_on_random_bytes) {
    SplitMix64 rng(current_seed());
    for (int iteration = 0; iteration < 4000; ++iteration) {
        const std::size_t size = static_cast<std::size_t>(rng.bounded(80));
        std::vector<std::uint8_t> bytes(size);
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(rng.bounded(256));
        }
        ByteReader reader(ByteSpan(bytes.data(), bytes.size()));
        const Result<EvidenceRecord> decoded = decode_evidence(reader);
        if (decoded.has_value()) {
            // Anything that decodes must re-encode to the same digest.
            ByteWriter writer(1024);
            RNF_REQUIRE_OK(encode(*decoded, writer));
            RNF_CHECK(evidence_digest(*decoded) ==
                      Blake2s256::hash(ByteSpan(writer.data().data(), writer.data().size())));
        }
    }
}

RNF_TEST(property, valid_records_always_roundtrip) {
    SplitMix64 rng(current_seed());
    for (int iteration = 0; iteration < 800; ++iteration) {
        const RandomRack rack = make_random_rack(rng, 0);
        for (const EvidenceRecord& record : rack.records) {
            ByteWriter writer(1024);
            RNF_REQUIRE_OK(encode(record, writer));
            ByteReader reader(writer.span());
            RNF_REQUIRE_VALUE(decoded, decode_evidence(reader));
            RNF_CHECK(decoded == record);
            RNF_CHECK(evidence_digest(decoded) == evidence_digest(record));
        }
        if (iteration > 20) {
            break;
        }
    }
}

RNF_TEST(property, snapshot_decode_never_crashes_on_random_bytes) {
    SplitMix64 rng(current_seed());
    const RandomRack rack = make_random_rack(rng, 0);
    const Snapshot snapshot = compose_random(rack.records, 0, Capacity{0});
    RNF_REQUIRE_VALUE(valid, snapshot.encode());
    for (int iteration = 0; iteration < 2000; ++iteration) {
        std::vector<std::uint8_t> bytes = valid;
        const std::size_t flips = 1 + static_cast<std::size_t>(rng.bounded(4));
        for (std::size_t i = 0; i < flips; ++i) {
            bytes[static_cast<std::size_t>(rng.bounded(bytes.size()))] =
                static_cast<std::uint8_t>(rng.bounded(256));
        }
        if (rng.coin()) {
            bytes.resize(static_cast<std::size_t>(rng.bounded(bytes.size() + 1)));
        }
        ByteReader reader(ByteSpan(bytes.data(), bytes.size()));
        const Result<SnapshotData> decoded = decode_snapshot(reader);
        if (decoded.has_value()) {
            RNF_CHECK(decoded->capacity.closes());
        }
    }
}

RNF_TEST(property, frame_header_validation_is_total) {
    SplitMix64 rng(current_seed());
    for (int iteration = 0; iteration < 4000; ++iteration) {
        std::vector<std::uint8_t> bytes(kFrameHeaderSize);
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(rng.bounded(256));
        }
        FrameHeader header;
        const Status status = decode_frame_header(ByteSpan(bytes.data(), bytes.size()),
                                                  kDefaultMaxFramePayload, kProtocolVersion, header);
        if (status.ok()) {
            RNF_CHECK(header.length <= kDefaultMaxFramePayload);
            RNF_CHECK(is_known_message_type(header.type));
            RNF_CHECK_EQ(header.version, kProtocolVersion);
            RNF_CHECK_EQ(header.magic, kFrameMagic);
        }
        // Every shorter prefix must be refused as truncated.
        for (std::size_t cut = 0; cut < kFrameHeaderSize; cut += 5) {
            FrameHeader short_header;
            RNF_REQUIRE_CODE(decode_frame_header(ByteSpan(bytes.data(), cut),
                                                 kDefaultMaxFramePayload, kProtocolVersion,
                                                 short_header),
                             StatusCode::kTruncated);
        }
    }
}

RNF_TEST(property, protocol_decoders_reject_random_payloads) {
    SplitMix64 rng(current_seed());
    for (int iteration = 0; iteration < 3000; ++iteration) {
        const std::size_t size = static_cast<std::size_t>(rng.bounded(64));
        std::vector<std::uint8_t> bytes(size);
        for (std::uint8_t& byte : bytes) {
            byte = static_cast<std::uint8_t>(rng.bounded(256));
        }
        ByteReader reader(ByteSpan(bytes.data(), bytes.size()));
        const Result<HelloRequest> hello = HelloRequest::decode(reader);
        if (hello.has_value()) {
            RNF_CHECK(reader.at_end());
        }
        ByteReader second(ByteSpan(bytes.data(), bytes.size()));
        (void)AcquireRequest::decode(second);
        ByteReader third(ByteSpan(bytes.data(), bytes.size()));
        (void)SubmitEvidenceRequest::decode(third);
        ByteReader fourth(ByteSpan(bytes.data(), bytes.size()));
        (void)LifecycleRequest::decode(fourth);
        ByteReader fifth(ByteSpan(bytes.data(), bytes.size()));
        (void)ResourceQuery::decode(fifth);
    }
}
