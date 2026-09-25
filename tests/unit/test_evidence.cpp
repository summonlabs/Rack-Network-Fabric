// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Evidence tests: canonical encoding, structural validation, and the ledger
// rules that make accepted evidence arrival-order independent.

#include <algorithm>
#include <string>
#include <vector>

#include "rnf/model/evidence.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/harness.hpp"

using namespace rnf;
using namespace rnf::test;

namespace {

EvidenceRecord roundtrip(const EvidenceRecord& record) {
    ByteWriter writer(4096);
    RNF_REQUIRE_OK(encode(record, writer));
    ByteReader reader(writer.span());
    RNF_REQUIRE_VALUE(decoded, decode_evidence(reader));
    RNF_CHECK(reader.at_end());
    return decoded;
}

}  // namespace

RNF_TEST(evidence, encode_decode_roundtrip_every_kind) {
    const std::vector<EvidenceRecord> samples = {
        member_record(3, 99, 4, kDiscoverySource, 12, Durability::kEphemeral),
        device_record(3, DeviceRole::kAppliance, Capacity{77}, Availability::kDegraded, 4,
                      kDiscoverySource, 13, Durability::kEphemeral, false),
        port_record(8, 3, 7, PortRole::kPeer, Capacity{12}, Availability::kDown,
                    AdminState::kDraining, 4, kDiscoverySource, 14),
        link_record(9, 8, 5, Capacity{33}, Availability::kMaintenance, 4, kDiscoverySource, 15,
                    Durability::kEphemeral, false, LinkKind::kLogical),
        attachment_record(4, 3, 8, "rack-7/host-9", 4, kDiscoverySource, 16),
        maintenance_record(MaintenanceKind::kMaintenance, ResourceKind::kPort, 8, "firmware", 4,
                           kDiscoverySource, 17),
        obligation_record(6, ResourceKind::kRack, 0, Capacity{120}, PrincipalId{5}, "pod-a", 4,
                          kImportedSource, 18),
    };
    for (const EvidenceRecord& sample : samples) {
        const EvidenceRecord decoded = roundtrip(sample);
        RNF_CHECK(decoded == sample);
        RNF_CHECK(evidence_digest(decoded) == evidence_digest(sample));
        RNF_CHECK(slot_of(decoded) == slot_of(sample));
    }
}

RNF_TEST(evidence, validation_refuses_malformed_records) {
    // Identity missing.
    MemberClaim no_device;
    no_device.rack = kRack;
    no_device.device = DeviceId(0);
    no_device.incarnation = 1;
    auto record = record_of(EvidenceKind::kMember, Durability::kSticky, 0, kOperatorSource, 1,
                            no_device);
    RNF_REQUIRE_CODE(validate_evidence(record), StatusCode::kInvalidIdentity);

    // Link with identical endpoints.
    LinkClaim self_link;
    self_link.link = LinkId(1);
    self_link.port_a = PortId(4);
    self_link.port_b = PortId(4);
    record = record_of(EvidenceKind::kLink, Durability::kSticky, 0, kOperatorSource, 1, self_link);
    RNF_REQUIRE_CODE(validate_evidence(record), StatusCode::kInvalidArgument);

    // Oversize host key.
    AttachmentClaim big;
    big.attachment = AttachmentId(1);
    big.device = DeviceId(1);
    big.port = PortId(1);
    big.host_key = std::string(4096, 'h');
    record = record_of(EvidenceKind::kAttachment, Durability::kSticky, 0, kOperatorSource, 1, big);
    RNF_REQUIRE_CODE(validate_evidence(record), StatusCode::kOversizeField);

    // Invalid UTF-8 in a text field.
    AttachmentClaim bad_text = big;
    bad_text.host_key = std::string("\xFF\xFE", 2);
    record = record_of(EvidenceKind::kAttachment, Durability::kSticky, 0, kOperatorSource, 1,
                       bad_text);
    RNF_REQUIRE_CODE(validate_evidence(record), StatusCode::kInvalidUnicode);

    // Rack wide maintenance with a resource id, and a resource target with none.
    MaintenanceClaim rack_with_id;
    rack_with_id.action = MaintenanceKind::kDrain;
    rack_with_id.target_kind = ResourceKind::kRack;
    rack_with_id.target_id = 5;
    record = record_of(EvidenceKind::kMaintenance, Durability::kSticky, 0, kOperatorSource, 1,
                       rack_with_id);
    RNF_REQUIRE_CODE(validate_evidence(record), StatusCode::kInvalidScope);

    MaintenanceClaim device_without_id;
    device_without_id.action = MaintenanceKind::kDrain;
    device_without_id.target_kind = ResourceKind::kDevice;
    device_without_id.target_id = 0;
    record = record_of(EvidenceKind::kMaintenance, Durability::kSticky, 0, kOperatorSource, 1,
                       device_without_id);
    RNF_REQUIRE_CODE(validate_evidence(record), StatusCode::kInvalidScope);

    // Obligation without a holder.
    ObligationClaim orphan;
    orphan.obligation = ObligationId(1);
    orphan.target_kind = ResourceKind::kDevice;
    orphan.target_id = 1;
    orphan.capacity = Capacity{5};
    orphan.holder = PrincipalId(0);
    record = record_of(EvidenceKind::kObligation, Durability::kSticky, 0, kImportedSource, 1,
                       orphan);
    RNF_REQUIRE_CODE(validate_evidence(record), StatusCode::kInvalidIdentity);
}

RNF_TEST(evidence, decode_refuses_truncated_and_hostile_payloads) {
    const EvidenceRecord sample = device_record(3, DeviceRole::kSwitch, Capacity{9});
    ByteWriter writer(512);
    RNF_REQUIRE_OK(encode(sample, writer));

    for (std::size_t cut = 0; cut < writer.size(); ++cut) {
        ByteReader reader(ByteSpan(writer.data().data(), cut));
        const Result<EvidenceRecord> decoded = decode_evidence(reader);
        RNF_CHECK(!decoded.has_value());
    }

    // A corrupted kind byte must not be accepted.
    std::vector<std::uint8_t> mutated = writer.data();
    mutated[0] = 0x7F;
    ByteReader reader(ByteSpan(mutated.data(), mutated.size()));
    RNF_REQUIRE_CODE(decode_evidence(reader).error(), StatusCode::kMalformedEncoding);

    // Trailing bytes after a valid record are the caller's problem: the reader
    // is left positioned, and the record itself still decodes.
    std::vector<std::uint8_t> padded = writer.data();
    padded.push_back(0xAA);
    ByteReader padded_reader(ByteSpan(padded.data(), padded.size()));
    RNF_REQUIRE_VALUE(decoded, decode_evidence(padded_reader));
    RNF_CHECK(decoded == sample);
    RNF_CHECK(!padded_reader.at_end());
}

RNF_TEST(evidence, ledger_insert_update_and_duplicate) {
    EvidenceLedger ledger;
    const EvidenceRecord first = device_record(1, DeviceRole::kSwitch, Capacity{100}, 
                                               Availability::kUp, 0, kOperatorSource, 5);
    RNF_CHECK(ledger.insert(first).outcome == InsertOutcome::kInserted);
    RNF_CHECK_EQ(ledger.size(), 1U);

    RNF_CHECK(ledger.insert(first).outcome == InsertOutcome::kDuplicate);
    RNF_CHECK_EQ(ledger.size(), 1U);

    const EvidenceRecord newer = device_record(1, DeviceRole::kSwitch, Capacity{200},
                                               Availability::kUp, 0, kOperatorSource, 6);
    const InsertReport updated = ledger.insert(newer);
    RNF_CHECK(updated.outcome == InsertOutcome::kUpdated);
    RNF_CHECK(updated.mutated);
    RNF_CHECK_EQ(ledger.size(), 1U);

    // Replaying the older record must not undo the newer one.
    const InsertReport replayed = ledger.insert(first);
    RNF_CHECK(replayed.outcome == InsertOutcome::kSuperseded);
    RNF_CHECK(!replayed.mutated);
    const EvidenceRecord* stored = ledger.find(slot_of(first));
    RNF_REQUIRE(stored != nullptr);
    RNF_CHECK(std::get<DeviceClaim>(stored->payload).capacity == Capacity{200});
    RNF_CHECK_EQ(ledger.superseded_count(), 1U);
}

RNF_TEST(evidence, ledger_conflict_is_deterministic_regardless_of_order) {
    const EvidenceRecord a = device_record(1, DeviceRole::kSwitch, Capacity{100}, 
                                           Availability::kUp, 0, kOperatorSource, 5);
    const EvidenceRecord b = device_record(1, DeviceRole::kHost, Capacity{100},
                                           Availability::kUp, 0, kOperatorSource, 5);
    RNF_REQUIRE(!(evidence_digest(a) == evidence_digest(b)));

    EvidenceLedger first_order;
    RNF_CHECK(first_order.insert(a).outcome == InsertOutcome::kInserted);
    RNF_CHECK(first_order.insert(b).outcome == InsertOutcome::kConflicted);
    EvidenceLedger second_order;
    RNF_CHECK(second_order.insert(b).outcome == InsertOutcome::kInserted);
    RNF_CHECK(second_order.insert(a).outcome == InsertOutcome::kConflicted);

    RNF_CHECK(first_order.digest() == second_order.digest());
    RNF_CHECK_EQ(first_order.conflict_count(), 1U);
    RNF_CHECK_EQ(second_order.conflict_count(), 1U);
    const EvidenceRecord* stored = first_order.find(slot_of(a));
    RNF_REQUIRE(stored != nullptr);
    const EvidenceRecord* other = second_order.find(slot_of(a));
    RNF_REQUIRE(other != nullptr);
    RNF_CHECK(*stored == *other);
}

RNF_TEST(evidence, ledger_digest_is_arrival_order_independent) {
    const std::vector<EvidenceRecord> records = linear_rack(0);
    EvidenceLedger forward;
    fill(forward, records);
    EvidenceLedger backward;
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        (void)backward.insert(*it);
    }
    RNF_CHECK(forward.digest() == backward.digest());
    RNF_CHECK_EQ(forward.size(), records.size());
    RNF_CHECK_EQ(backward.size(), records.size());

    // A different multiset must give a different digest.
    std::vector<EvidenceRecord> altered = records;
    altered.pop_back();
    EvidenceLedger reduced;
    fill(reduced, altered);
    RNF_CHECK(!(reduced.digest() == forward.digest()));
}

RNF_TEST(evidence, ledger_bounds_are_enforced) {
    EvidenceLimits limits;
    limits.max_slots = 4;
    limits.max_sources = 2;
    EvidenceLedger ledger(limits);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        RNF_CHECK(ledger.insert(device_record(i, DeviceRole::kHost, Capacity{1})).outcome ==
                  InsertOutcome::kInserted);
    }
    const InsertReport refused = ledger.insert(device_record(5, DeviceRole::kHost, Capacity{1}));
    RNF_CHECK(refused.outcome == InsertOutcome::kRefused);
    RNF_CHECK(refused.status.code() == StatusCode::kResourceExhausted);

    EvidenceLedger sources(limits);
    RNF_CHECK(sources.insert(device_record(1, DeviceRole::kHost, Capacity{1}, Availability::kUp, 0,
                                           SourceId{1}, 1))
                  .outcome == InsertOutcome::kInserted);
    RNF_CHECK(sources.insert(device_record(2, DeviceRole::kHost, Capacity{1}, Availability::kUp, 0,
                                           SourceId{2}, 1))
                  .outcome == InsertOutcome::kInserted);
    const InsertReport third = sources.insert(device_record(3, DeviceRole::kHost, Capacity{1},
                                                            Availability::kUp, 0, SourceId{3}, 1));
    RNF_CHECK(third.outcome == InsertOutcome::kRefused);
    RNF_CHECK(third.status.code() == StatusCode::kResourceExhausted);
}

RNF_TEST(evidence, ledger_clear_resets_every_counter) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    RNF_CHECK(ledger.size() > 0);
    ledger.clear();
    RNF_CHECK_EQ(ledger.size(), 0U);
    RNF_CHECK_EQ(ledger.source_count(), 0U);
    RNF_CHECK_EQ(ledger.conflict_count(), 0U);
    RNF_CHECK_EQ(ledger.superseded_count(), 0U);
    RNF_CHECK_EQ(ledger.digest(), Blake2s256::hash(std::string_view("rnf.ledger.v1")));
}
