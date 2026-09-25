// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Composition tests: membership authority, eligibility, maintenance monotonicity,
// imported obligations, capacity closure, path enumeration and determinism.

#include <algorithm>
#include <string>
#include <vector>

#include "rnf/compose/composer.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/harness.hpp"

using namespace rnf;
using namespace rnf::test;

namespace {

Snapshot compose_or_fail(const EvidenceLedger& ledger, const RackConfig& config,
                         const ComposeInput& input) {
    Result<ComposeOutcome> outcome = compose(ledger, config, input);
    if (!outcome.has_value()) {
        RNF_REQUIRE(false);
    }
    return outcome->snapshot;
}

bool has_diagnostic(const Snapshot& snapshot, DiagnosticKind kind, std::uint64_t subject) {
    for (const Diagnostic& diagnostic : snapshot->diagnostics) {
        if (diagnostic.kind == kind && diagnostic.subject_id == subject) {
            return true;
        }
    }
    return false;
}

std::size_t diagnostic_count(const Snapshot& snapshot, DiagnosticKind kind) {
    std::size_t count = 0;
    for (const Diagnostic& diagnostic : snapshot->diagnostics) {
        if (diagnostic.kind == kind) {
            ++count;
        }
    }
    return count;
}

}  // namespace

RNF_TEST(compose, linear_rack_is_fully_eligible) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK(snapshot.valid());
    RNF_CHECK_EQ(snapshot->devices.size(), 2U);
    RNF_CHECK_EQ(snapshot->ports.size(), 4U);
    RNF_CHECK_EQ(snapshot->links.size(), 2U);
    RNF_CHECK_EQ(snapshot->attachments.size(), 1U);
    RNF_CHECK_EQ(diagnostic_count(snapshot, DiagnosticKind::kStaleEvidence), 0U);
    RNF_CHECK_EQ(diagnostic_count(snapshot, DiagnosticKind::kOutOfRack), 0U);
    for (const DeviceRecord& device : snapshot->devices) {
        RNF_CHECK(device.eligibility == Eligibility::kEligible);
    }
    for (const LinkRecord& link : snapshot->links) {
        RNF_CHECK(link.eligibility == Eligibility::kEligible);
    }
    RNF_CHECK(snapshot.find(ResourceRef{ResourceKind::kRack, kRack.value()}) != nullptr);
    RNF_CHECK(snapshot.find(ResourceRef{ResourceKind::kDevice, 1}) != nullptr);
    RNF_CHECK(snapshot.find(ResourceRef{ResourceKind::kPort, 10}) != nullptr);
    RNF_CHECK(snapshot.find(ResourceRef{ResourceKind::kLink, 100}) != nullptr);
    RNF_CHECK(snapshot.find(ResourceRef{ResourceKind::kDevice, 999}) == nullptr);
}

RNF_TEST(compose, out_of_rack_membership_never_gains_authority) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    // A device that only ever claims to be a member of a different rack.
    (void)ledger.insert(member_record(3, 33, 0, kDiscoverySource, 1, Durability::kSticky,
                                      RackId{99}));
    (void)ledger.insert(device_record(3, DeviceRole::kSwitch, Capacity{5000},
                                      Availability::kUp, 0, kDiscoverySource, 2));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK(snapshot.find_device(DeviceId(3)) == nullptr);
    RNF_CHECK(snapshot.find(ResourceRef{ResourceKind::kDevice, 3}) == nullptr);
    RNF_CHECK(has_diagnostic(snapshot, DiagnosticKind::kOutOfRack, 3));
    // The rack's total capacity must not include the foreign device.
    RNF_CHECK_EQ(snapshot->capacity.total.units, 1500U);
}

RNF_TEST(compose, device_without_membership_is_excluded) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(device_record(4, DeviceRole::kHost, Capacity{900}, Availability::kUp, 0,
                                      kDiscoverySource, 1));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK(snapshot.find_device(DeviceId(4)) == nullptr);
    RNF_CHECK(has_diagnostic(snapshot, DiagnosticKind::kUnknownMember, 4));
    RNF_CHECK_EQ(snapshot->capacity.total.units, 1500U);
}

RNF_TEST(compose, port_on_foreign_device_is_refused) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(port_record(99, 77, 0, PortRole::kAccess, Capacity{100}));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK(snapshot.find_port(PortId(99)) == nullptr);
    RNF_CHECK(has_diagnostic(snapshot, DiagnosticKind::kOutOfRack, 99));
}

RNF_TEST(compose, unknown_availability_is_not_up) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(port_record(10, 1, 0, PortRole::kUplink, Capacity{400},
                                    Availability::kUnknown, AdminState::kEnabled, 0,
                                    kOperatorSource, 50));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    const PortRecord* port = snapshot.find_port(PortId(10));
    RNF_REQUIRE(port != nullptr);
    RNF_CHECK(port->availability == Availability::kUnknown);
    RNF_CHECK(port->eligibility == Eligibility::kIneligible);
    const LinkRecord* link = snapshot.find_link(LinkId(100));
    RNF_REQUIRE(link != nullptr);
    RNF_CHECK(link->eligibility == Eligibility::kIneligible);
}

RNF_TEST(compose, unknown_capacity_is_never_success) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(device_record(2, DeviceRole::kHost, Capacity{0}, Availability::kUp, 0,
                                      kOperatorSource, 60, Durability::kSticky, false));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    const DeviceRecord* device = snapshot.find_device(DeviceId(2));
    RNF_REQUIRE(device != nullptr);
    RNF_CHECK(!device->capacity_known);
    RNF_CHECK(device->eligibility == Eligibility::kIneligible);
    RNF_CHECK(has_diagnostic(snapshot, DiagnosticKind::kUnknownCapacity, 2));
    RNF_CHECK_EQ(snapshot->capacity.total.units, 1000U);
    RNF_CHECK_EQ(snapshot->capacity.unavailable.units, 0U);
}

RNF_TEST(compose, down_device_capacity_becomes_unavailable) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(device_record(2, DeviceRole::kHost, Capacity{500}, Availability::kDown, 0,
                                      kOperatorSource, 61));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK_EQ(snapshot->capacity.total.units, 1500U);
    RNF_CHECK_EQ(snapshot->capacity.unavailable.units, 500U);
    RNF_CHECK_EQ(snapshot->capacity.usable.units, 1000U);
    RNF_CHECK(snapshot->capacity.closes());
}

RNF_TEST(compose, ephemeral_evidence_does_not_carry_forward) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(port_record(11, 1, 1, PortRole::kAccess, Capacity{400},
                                    Availability::kDown, AdminState::kEnabled, 0, kDiscoverySource,
                                    70, Durability::kEphemeral));
    const Snapshot at_zero = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    const PortRecord* port = at_zero.find_port(PortId(11));
    RNF_REQUIRE(port != nullptr);
    RNF_CHECK(port->availability == Availability::kDown);

    // At generation 1 the ephemeral discovery record says nothing, so the
    // operator's sticky view of the port applies instead of a silent "up".
    const Snapshot at_one = compose_or_fail(ledger, linear_rack_config(), compose_input(1));
    const PortRecord* later = at_one.find_port(PortId(11));
    RNF_REQUIRE(later != nullptr);
    RNF_CHECK(later->availability == Availability::kUp);
    RNF_CHECK(diagnostic_count(at_one, DiagnosticKind::kStaleEvidence) > 0);
}

RNF_TEST(compose, future_generation_evidence_is_not_applied) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(member_record(9, 99, 5, kDiscoverySource, 1));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(1));
    RNF_CHECK(snapshot.find_device(DeviceId(9)) == nullptr);
    RNF_CHECK(has_diagnostic(snapshot, DiagnosticKind::kStaleEvidence, 9));
}

RNF_TEST(compose, maintenance_excludes_and_survives_replay) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(maintenance_record(MaintenanceKind::kDrain, ResourceKind::kDevice, 2,
                                           "planned", 0, kOperatorSource, 100));
    const Snapshot drained = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK(drained.find(ResourceRef{ResourceKind::kDevice, 2})->maintenance);
    RNF_CHECK(drained.find(ResourceRef{ResourceKind::kDevice, 2})->eligibility ==
              Eligibility::kIneligible);

    // A replayed older clear must not lift the exclusion.
    (void)ledger.insert(maintenance_record(MaintenanceKind::kClear, ResourceKind::kDevice, 2, "",
                                           0, kOperatorSource, 50));
    const Snapshot still_drained = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK(still_drained.find(ResourceRef{ResourceKind::kDevice, 2})->maintenance);

    // A genuinely newer clear does lift it.
    (void)ledger.insert(maintenance_record(MaintenanceKind::kClear, ResourceKind::kDevice, 2,
                                           "done", 0, kOperatorSource, 200));
    const Snapshot cleared = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK(!cleared.find(ResourceRef{ResourceKind::kDevice, 2})->maintenance);
    RNF_CHECK(cleared.find(ResourceRef{ResourceKind::kDevice, 2})->eligibility ==
              Eligibility::kEligible);
}

RNF_TEST(compose, maintenance_from_one_source_cannot_be_cleared_by_another) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(maintenance_record(MaintenanceKind::kDrain, ResourceKind::kDevice, 2,
                                           "operator", 0, kOperatorSource, 100));
    (void)ledger.insert(maintenance_record(MaintenanceKind::kClear, ResourceKind::kDevice, 2, "",
                                           0, kDiscoverySource, 999));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK(snapshot.find(ResourceRef{ResourceKind::kDevice, 2})->maintenance);
    RNF_CHECK(has_diagnostic(snapshot, DiagnosticKind::kMaintenanceExclusion, 2));
}

RNF_TEST(compose, rack_wide_maintenance_excludes_everything) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(maintenance_record(MaintenanceKind::kMaintenance, ResourceKind::kRack, 0,
                                           "whole rack", 0, kOperatorSource, 100));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    for (const ResourceState& state : snapshot->resources) {
        RNF_CHECK(!(state.eligibility == Eligibility::kEligible));
        RNF_CHECK(state.maintenance);
    }
    RNF_CHECK_EQ(snapshot->capacity.usable.units, 0U);
}

RNF_TEST(compose, obligations_reduce_capacity_and_close_exactly) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(obligation_record(1, ResourceKind::kDevice, 1, Capacity{300},
                                          PrincipalId{5}, "pod-a"));
    (void)ledger.insert(obligation_record(2, ResourceKind::kRack, 0, Capacity{100},
                                          PrincipalId{6}, "pod-b"));
    RackConfig config = linear_rack_config();
    config.headroom_floor = Capacity{50};
    const Snapshot snapshot = compose_or_fail(ledger, config, compose_input(0));
    RNF_CHECK_EQ(snapshot->capacity.total.units, 1500U);
    RNF_CHECK_EQ(snapshot->capacity.unavailable.units, 0U);
    RNF_CHECK_EQ(snapshot->capacity.obligated.units, 400U);
    RNF_CHECK_EQ(snapshot->capacity.headroom_floor.units, 50U);
    RNF_CHECK_EQ(snapshot->capacity.uncommitted.units, 1050U);
    RNF_CHECK_EQ(snapshot->capacity.deficit.units, 0U);
    RNF_CHECK(snapshot->capacity.closes());

    const ResourceState* device = snapshot.find(ResourceRef{ResourceKind::kDevice, 1});
    RNF_REQUIRE(device != nullptr);
    RNF_CHECK_EQ(device->obligated.units, 300U);
}

RNF_TEST(compose, over_commit_is_reported_as_a_deficit_not_clamped) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(obligation_record(1, ResourceKind::kRack, 0, Capacity{2000},
                                          PrincipalId{5}, "pod-a"));
    RackConfig config = linear_rack_config();
    config.headroom_floor = Capacity{100};
    const Snapshot snapshot = compose_or_fail(ledger, config, compose_input(0));
    RNF_CHECK_EQ(snapshot->capacity.usable.units, 1500U);
    RNF_CHECK_EQ(snapshot->capacity.obligated.units, 2000U);
    RNF_CHECK_EQ(snapshot->capacity.uncommitted.units, 0U);
    RNF_CHECK_EQ(snapshot->capacity.deficit.units, 600U);
    RNF_CHECK(snapshot->capacity.closes());
    RNF_CHECK(diagnostic_count(snapshot, DiagnosticKind::kCapacityOvercommit) > 0);
}

RNF_TEST(compose, out_of_rack_obligation_is_refused) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(obligation_record(1, ResourceKind::kDevice, 4242, Capacity{10},
                                          PrincipalId{5}, "pod-a"));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK_EQ(snapshot->obligations.size(), 0U);
    RNF_CHECK_EQ(snapshot->capacity.obligated.units, 0U);
    RNF_CHECK(has_diagnostic(snapshot, DiagnosticKind::kOutOfRack, 4242));
}

RNF_TEST(compose, incomplete_link_is_not_eligible) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(link_record(102, 10, 999, Capacity{100}, Availability::kUp, 0,
                                    kOperatorSource, 90));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    const LinkRecord* link = snapshot.find_link(LinkId(102));
    RNF_REQUIRE(link != nullptr);
    RNF_CHECK(link->eligibility == Eligibility::kIncomplete);
    RNF_CHECK(has_diagnostic(snapshot, DiagnosticKind::kIncompleteTopology, 102));
}

RNF_TEST(compose, link_capacity_is_bounded_by_its_ports) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    // The link advertises more than its access port can carry.
    (void)ledger.insert(link_record(100, 10, 20, Capacity{5000}, Availability::kUp, 0,
                                    kOperatorSource, 91));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    const LinkRecord* link = snapshot.find_link(LinkId(100));
    RNF_REQUIRE(link != nullptr);
    RNF_CHECK_EQ(link->capacity.units, 200U);
}

RNF_TEST(compose, paths_are_enumerated_deterministically) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK(!snapshot->paths.empty());
    for (std::size_t i = 1; i < snapshot->paths.size(); ++i) {
        const PathRecord& previous = snapshot->paths[i - 1];
        const PathRecord& current = snapshot->paths[i];
        RNF_CHECK(!(current.start_port < previous.start_port));
    }
    for (const PathRecord& path : snapshot->paths) {
        RNF_CHECK(!path.links.empty());
        RNF_CHECK(path.eligibility == Eligibility::kEligible);
        RNF_CHECK(path.bottleneck_known);
    }
    // Enumerating twice must give byte-identical results.
    const Snapshot again = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK(again.digest() == snapshot.digest());
}

RNF_TEST(compose, draining_a_link_removes_its_paths) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    const Snapshot before = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_REQUIRE(!before->paths.empty());
    (void)ledger.insert(maintenance_record(MaintenanceKind::kDrain, ResourceKind::kPort, 20,
                                           "drain access", 0, kOperatorSource, 300));
    const Snapshot after = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    for (const PathRecord& path : after->paths) {
        RNF_CHECK(!(path.start_port == PortId(20)));
        RNF_CHECK(!(path.end_port == PortId(20)));
    }
}

RNF_TEST(compose, duplicate_device_port_index_is_a_conflict) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(port_record(13, 1, 0, PortRole::kAccess, Capacity{400}));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_CHECK(has_diagnostic(snapshot, DiagnosticKind::kConflictingEvidence, 13));
}

RNF_TEST(compose, conflicting_membership_makes_the_device_unusable) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    (void)ledger.insert(member_record(2, 999, 0, kDiscoverySource, 1));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    const DeviceRecord* device = snapshot.find_device(DeviceId(2));
    RNF_REQUIRE(device != nullptr);
    RNF_CHECK(device->eligibility == Eligibility::kIncomplete);
    RNF_CHECK(has_diagnostic(snapshot, DiagnosticKind::kConflictingEvidence, 2));
}

RNF_TEST(compose, digest_is_content_addressed_and_order_independent) {
    const std::vector<EvidenceRecord> records = linear_rack(0);
    EvidenceLedger forward;
    fill(forward, records);
    EvidenceLedger shuffled;
    std::vector<EvidenceRecord> copy = records;
    std::reverse(copy.begin(), copy.end());
    // Interleave two different orders to show that only the multiset matters.
    for (std::size_t i = 0; i < copy.size(); ++i) {
        (void)shuffled.insert(copy[i]);
        (void)shuffled.insert(records[i]);
    }
    const Snapshot a = compose_or_fail(forward, linear_rack_config(), compose_input(0));
    const Snapshot b = compose_or_fail(shuffled, linear_rack_config(), compose_input(0));
    RNF_CHECK(a.digest() == b.digest());
    RNF_CHECK(a->member_set_digest == b->member_set_digest);

    // Changing anything visible changes the digest.
    EvidenceLedger changed;
    fill(changed, records);
    (void)changed.insert(device_record(1, DeviceRole::kSwitch, Capacity{1001}, Availability::kUp,
                                       0, kOperatorSource, 1000));
    const Snapshot c = compose_or_fail(changed, linear_rack_config(), compose_input(0));
    RNF_CHECK(!(c.digest() == a.digest()));
}

RNF_TEST(compose, snapshot_encoding_roundtrips) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));
    RNF_REQUIRE_VALUE(bytes, snapshot.encode());
    ByteReader reader(ByteSpan(bytes.data(), bytes.size()));
    RNF_REQUIRE_VALUE(decoded, decode_snapshot(reader));
    RNF_CHECK(decoded == snapshot.data());
    RNF_CHECK(snapshot_digest(decoded) == snapshot.digest());
    RNF_CHECK(decoded.capacity.closes());

    // Every truncation must be refused rather than half decoded.
    for (std::size_t cut = 0; cut < bytes.size(); cut += 7) {
        ByteReader short_reader(ByteSpan(bytes.data(), cut));
        RNF_CHECK(!decode_snapshot(short_reader).has_value());
    }
}

RNF_TEST(compose, scope_expansion_covers_the_right_resources) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    const Snapshot snapshot = compose_or_fail(ledger, linear_rack_config(), compose_input(0));

    RNF_REQUIRE_VALUE(device_scope, expand_scope(snapshot, ResourceRef{ResourceKind::kDevice, 1}));
    RNF_CHECK(std::find(device_scope.begin(), device_scope.end(),
                        ResourceRef{ResourceKind::kPort, 10}) != device_scope.end());
    RNF_CHECK(std::find(device_scope.begin(), device_scope.end(),
                        ResourceRef{ResourceKind::kLink, 100}) != device_scope.end());
    RNF_CHECK(std::find(device_scope.begin(), device_scope.end(),
                        ResourceRef{ResourceKind::kDevice, 2}) == device_scope.end());

    RNF_REQUIRE_VALUE(port_scope, expand_scope(snapshot, ResourceRef{ResourceKind::kPort, 10}));
    RNF_CHECK(std::find(port_scope.begin(), port_scope.end(),
                        ResourceRef{ResourceKind::kLink, 100}) != port_scope.end());

    RNF_REQUIRE_VALUE(link_scope, expand_scope(snapshot, ResourceRef{ResourceKind::kLink, 100}));
    RNF_CHECK(std::find(link_scope.begin(), link_scope.end(),
                        ResourceRef{ResourceKind::kPort, 20}) != link_scope.end());

    RNF_REQUIRE_VALUE(rack_scope, expand_scope(snapshot, ResourceRef{ResourceKind::kRack,
                                                                    kRack.value()}));
    RNF_CHECK_EQ(rack_scope.size(), snapshot->resources.size());

    RNF_REQUIRE_CODE(expand_scope(snapshot, ResourceRef{ResourceKind::kDevice, 999}),
                     StatusCode::kNotAMember);
    RNF_REQUIRE_CODE(expand_scope(snapshot, ResourceRef{ResourceKind::kRack, 12345}),
                     StatusCode::kOutOfRack);

    // A device scope always intersects the rack scope, and the two devices in
    // this rack intersect each other because they share link 100. That is the
    // property that makes an exclusive grant on one device block the other.
    RNF_CHECK(scopes_intersect(device_scope, rack_scope));
    RNF_REQUIRE_VALUE(other, expand_scope(snapshot, ResourceRef{ResourceKind::kDevice, 2}));
    RNF_CHECK(scopes_intersect(device_scope, other));
    RNF_CHECK(std::find(device_scope.begin(), device_scope.end(),
                        ResourceRef{ResourceKind::kLink, 100}) != device_scope.end());
    RNF_CHECK(std::find(other.begin(), other.end(), ResourceRef{ResourceKind::kLink, 100}) !=
              other.end());
    // Genuinely disjoint scopes do not intersect: port 11 reaches only link 101
    // and port 20 only link 100.
    RNF_REQUIRE_VALUE(port_eleven, expand_scope(snapshot, ResourceRef{ResourceKind::kPort, 11}));
    RNF_REQUIRE_VALUE(port_twenty, expand_scope(snapshot, ResourceRef{ResourceKind::kPort, 20}));
    RNF_CHECK(!scopes_intersect(port_eleven, port_twenty));
}
