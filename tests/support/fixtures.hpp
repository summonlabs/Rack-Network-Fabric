// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Shared fixtures for the unit, property and differential tests: small,
// completely explicit racks built from evidence records.

#ifndef RNF_TEST_FIXTURES_HPP
#define RNF_TEST_FIXTURES_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "rnf/compose/composer.hpp"
#include "rnf/model/evidence.hpp"

namespace rnf::test {

inline constexpr SourceId kOperatorSource{1};
inline constexpr SourceId kDiscoverySource{2};
inline constexpr SourceId kImportedSource{3};
inline constexpr RackId kRack{7};

/// Build one evidence record with an explicit provenance.
[[nodiscard]] inline EvidenceRecord record_of(EvidenceKind kind, Durability durability,
                                              std::uint64_t generation, SourceId source,
                                              std::uint64_t sequence, EvidencePayload payload) {
    EvidenceRecord record;
    record.kind = kind;
    record.durability = durability;
    record.generation = TopologyGeneration{generation};
    record.provenance.source = source;
    record.provenance.kind = source == kOperatorSource ? SourceKind::kOperator
                                                       : SourceKind::kDiscovery;
    record.provenance.sequence = sequence;
    record.provenance.observed_at_ms = sequence * 1000U;
    record.payload = std::move(payload);
    return record;
}

[[nodiscard]] inline EvidenceRecord member_record(std::uint64_t device,
                                                  MemberIncarnation incarnation,
                                                  std::uint64_t generation = 0,
                                                  SourceId source = kOperatorSource,
                                                  std::uint64_t sequence = 1,
                                                  Durability durability = Durability::kSticky,
                                                  RackId rack = kRack) {
    MemberClaim claim;
    claim.rack = rack;
    claim.device = DeviceId(device);
    claim.incarnation = incarnation;
    return record_of(EvidenceKind::kMember, durability, generation, source, sequence, claim);
}

[[nodiscard]] inline EvidenceRecord device_record(std::uint64_t device, DeviceRole role,
                                                  Capacity capacity,
                                                  Availability availability = Availability::kUp,
                                                  std::uint64_t generation = 0,
                                                  SourceId source = kOperatorSource,
                                                  std::uint64_t sequence = 1,
                                                  Durability durability = Durability::kSticky,
                                                  bool capacity_known = true) {
    DeviceClaim claim;
    claim.device = DeviceId(device);
    claim.role = role;
    claim.capacity = capacity;
    claim.capacity_known = capacity_known;
    claim.availability = availability;
    return record_of(EvidenceKind::kDevice, durability, generation, source, sequence, claim);
}

[[nodiscard]] inline EvidenceRecord port_record(std::uint64_t port, std::uint64_t device,
                                                std::uint16_t index, PortRole role,
                                                Capacity capacity,
                                                Availability availability = Availability::kUp,
                                                AdminState admin = AdminState::kEnabled,
                                                std::uint64_t generation = 0,
                                                SourceId source = kOperatorSource,
                                                std::uint64_t sequence = 1,
                                                Durability durability = Durability::kSticky,
                                                bool capacity_known = true) {
    PortClaim claim;
    claim.port = PortId(port);
    claim.device = DeviceId(device);
    claim.index = index;
    claim.role = role;
    claim.capacity = capacity;
    claim.capacity_known = capacity_known;
    claim.availability = availability;
    claim.admin = admin;
    return record_of(EvidenceKind::kPort, durability, generation, source, sequence, claim);
}

[[nodiscard]] inline EvidenceRecord link_record(std::uint64_t link, std::uint64_t port_a,
                                                std::uint64_t port_b, Capacity capacity,
                                                Availability availability = Availability::kUp,
                                                std::uint64_t generation = 0,
                                                SourceId source = kOperatorSource,
                                                std::uint64_t sequence = 1,
                                                Durability durability = Durability::kSticky,
                                                bool capacity_known = true,
                                                LinkKind kind = LinkKind::kPhysical) {
    LinkClaim claim;
    claim.link = LinkId(link);
    claim.port_a = PortId(port_a);
    claim.port_b = PortId(port_b);
    claim.kind = kind;
    claim.capacity = capacity;
    claim.capacity_known = capacity_known;
    claim.availability = availability;
    return record_of(EvidenceKind::kLink, durability, generation, source, sequence, claim);
}

[[nodiscard]] inline EvidenceRecord attachment_record(std::uint64_t attachment,
                                                      std::uint64_t device, std::uint64_t port,
                                                      std::string host_key,
                                                      std::uint64_t generation = 0,
                                                      SourceId source = kOperatorSource,
                                                      std::uint64_t sequence = 1,
                                                      Durability durability = Durability::kSticky) {
    AttachmentClaim claim;
    claim.attachment = AttachmentId(attachment);
    claim.device = DeviceId(device);
    claim.port = PortId(port);
    claim.host_key = std::move(host_key);
    return record_of(EvidenceKind::kAttachment, durability, generation, source, sequence, claim);
}

[[nodiscard]] inline EvidenceRecord maintenance_record(MaintenanceKind action, ResourceKind kind,
                                                       std::uint64_t id, std::string reason,
                                                       std::uint64_t generation = 0,
                                                       SourceId source = kOperatorSource,
                                                       std::uint64_t sequence = 1,
                                                       Durability durability = Durability::kSticky) {
    MaintenanceClaim claim;
    claim.action = action;
    claim.target_kind = kind;
    claim.target_id = id;
    claim.reason = std::move(reason);
    return record_of(EvidenceKind::kMaintenance, durability, generation, source, sequence, claim);
}

[[nodiscard]] inline EvidenceRecord obligation_record(std::uint64_t obligation, ResourceKind kind,
                                                      std::uint64_t id, Capacity capacity,
                                                      PrincipalId holder, std::string authority,
                                                      std::uint64_t generation = 0,
                                                      SourceId source = kImportedSource,
                                                      std::uint64_t sequence = 1,
                                                      Durability durability = Durability::kSticky) {
    ObligationClaim claim;
    claim.obligation = ObligationId(obligation);
    claim.target_kind = kind;
    claim.target_id = id;
    claim.capacity = capacity;
    claim.holder = holder;
    claim.authority = std::move(authority);
    return record_of(EvidenceKind::kObligation, durability, generation, source, sequence, claim);
}

/// A two device rack: device 1 is a switch with three ports, device 2 is a host
/// with one port, and a single link joins them.
[[nodiscard]] inline std::vector<EvidenceRecord> linear_rack(std::uint64_t generation = 0) {
    std::vector<EvidenceRecord> records;
    records.push_back(member_record(1, 11, generation, kOperatorSource, 1));
    records.push_back(member_record(2, 22, generation, kOperatorSource, 2));
    records.push_back(device_record(1, DeviceRole::kSwitch, Capacity{1000}, Availability::kUp,
                                    generation, kOperatorSource, 3));
    records.push_back(device_record(2, DeviceRole::kHost, Capacity{500}, Availability::kUp,
                                    generation, kOperatorSource, 4));
    records.push_back(port_record(10, 1, 0, PortRole::kUplink, Capacity{400}, Availability::kUp,
                                  AdminState::kEnabled, generation, kOperatorSource, 5));
    records.push_back(port_record(11, 1, 1, PortRole::kAccess, Capacity{400}, Availability::kUp,
                                  AdminState::kEnabled, generation, kOperatorSource, 6));
    records.push_back(port_record(12, 1, 2, PortRole::kFabric, Capacity{400}, Availability::kUp,
                                  AdminState::kEnabled, generation, kOperatorSource, 7));
    records.push_back(port_record(20, 2, 0, PortRole::kAccess, Capacity{200}, Availability::kUp,
                                  AdminState::kEnabled, generation, kOperatorSource, 8));
    records.push_back(link_record(100, 10, 20, Capacity{200}, Availability::kUp, generation,
                                  kOperatorSource, 9));
    records.push_back(link_record(101, 11, 12, Capacity{150}, Availability::kUp, generation,
                                  kOperatorSource, 10));
    records.push_back(attachment_record(200, 2, 20, "host-a", generation, kOperatorSource, 11));
    return records;
}

[[nodiscard]] inline RackConfig linear_rack_config() {
    RackConfig config;
    config.rack = kRack;
    config.name = "test-rack";
    config.headroom_floor = Capacity{0};
    return config;
}

[[nodiscard]] inline ComposeInput compose_input(std::uint64_t generation = 0,
                                                std::uint64_t epoch = 0,
                                                LifecycleState lifecycle = LifecycleState::kActive) {
    ComposeInput input;
    input.generation = TopologyGeneration{generation};
    input.epoch = RackEpoch{epoch};
    input.lifecycle = lifecycle;
    return input;
}

/// Fill a ledger with every record of a fixture.
inline void fill(EvidenceLedger& ledger, const std::vector<EvidenceRecord>& records) {
    for (const EvidenceRecord& record : records) {
        (void)ledger.insert(record);
    }
}

}  // namespace rnf::test

#endif  // RNF_TEST_FIXTURES_HPP
