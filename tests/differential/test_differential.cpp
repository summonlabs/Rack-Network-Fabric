// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Differential tests. Each case runs two independent implementations of the
// same rule - the indexed production path and a deliberately naive reference
// model - and requires them to agree.

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "rnf/authority/registry.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/harness.hpp"

using namespace rnf;
using namespace rnf::test;

namespace {

/// Reference model for the capacity ledger, written from the raw evidence
/// rather than from the composer's intermediate structures.
struct ReferenceCapacity {
    std::uint64_t total = 0;
    std::uint64_t unavailable = 0;
    std::uint64_t usable = 0;
    std::uint64_t obligated = 0;
    std::uint64_t headroom = 0;
    std::uint64_t uncommitted = 0;
    std::uint64_t deficit = 0;
};

ReferenceCapacity reference_capacity(const std::vector<EvidenceRecord>& records, RackId rack,
                                     Capacity headroom) {
    struct MemberState {
        std::uint64_t incarnation = 0;
        bool indeterminate = false;
    };
    struct DeviceState {
        bool present = false;
        std::uint64_t capacity = 0;
        bool capacity_known = false;
        Availability availability = Availability::kUnknown;
        std::uint64_t source = 0;
    };
    std::map<std::uint64_t, MemberState> members;
    std::map<std::uint64_t, DeviceState> devices;
    std::set<std::uint64_t> drained_devices;
    bool rack_drained = false;
    std::uint64_t obligated = 0;

    for (const EvidenceRecord& record : records) {
        const bool sticky = record.durability == Durability::kSticky;
        if (!sticky && record.generation.value != 0) {
            continue;
        }
        if (record.generation.value > 0) {
            continue;
        }
        switch (record.kind) {
            case EvidenceKind::kMember: {
                const auto& claim = std::get<MemberClaim>(record.payload);
                if (!(claim.rack == rack)) {
                    continue;
                }
                auto& state = members[claim.device.value()];
                if (state.incarnation != 0 && state.incarnation != claim.incarnation) {
                    state.indeterminate = true;
                }
                state.incarnation = std::max(state.incarnation, claim.incarnation);
                break;
            }
            case EvidenceKind::kDevice: {
                const auto& claim = std::get<DeviceClaim>(record.payload);
                if (devices.find(claim.device.value()) != devices.end() &&
                    devices[claim.device.value()].source >= record.provenance.sequence) {
                    break;
                }
                DeviceState& state = devices[claim.device.value()];
                state.present = true;
                state.capacity = claim.capacity.units;
                state.capacity_known = claim.capacity_known;
                state.availability = claim.availability;
                state.source = record.provenance.sequence;
                break;
            }
            case EvidenceKind::kMaintenance: {
                const auto& claim = std::get<MaintenanceClaim>(record.payload);
                if (claim.action == MaintenanceKind::kClear) {
                    break;
                }
                if (claim.target_kind == ResourceKind::kRack) {
                    rack_drained = true;
                } else if (claim.target_kind == ResourceKind::kDevice) {
                    drained_devices.insert(claim.target_id);
                }
                break;
            }
            case EvidenceKind::kObligation: {
                const auto& claim = std::get<ObligationClaim>(record.payload);
                if (claim.target_kind == ResourceKind::kRack) {
                    obligated += claim.capacity.units;
                } else if (claim.target_kind == ResourceKind::kDevice &&
                           members.find(claim.target_id) != members.end()) {
                    obligated += claim.capacity.units;
                } else if (claim.target_kind == ResourceKind::kLink) {
                    obligated += claim.capacity.units;
                }
                break;
            }
            default:
                break;
        }
    }

    ReferenceCapacity reference;
    for (const auto& entry : members) {
        const std::uint64_t device = entry.first;
        const auto device_it = devices.find(device);
        if (device_it == devices.end() || !device_it->second.capacity_known) {
            continue;
        }
        const std::uint64_t capacity = device_it->second.capacity;
        reference.total += capacity;
        const bool usable = !entry.second.indeterminate && !rack_drained &&
                            drained_devices.find(device) == drained_devices.end() &&
                            (device_it->second.availability == Availability::kUp ||
                             device_it->second.availability == Availability::kDegraded);
        if (!usable) {
            reference.unavailable += capacity;
        }
    }
    reference.usable = reference.total - reference.unavailable;
    reference.obligated = obligated;
    reference.headroom = headroom.units;
    const std::uint64_t claimed = reference.obligated + reference.headroom;
    if (claimed > reference.usable) {
        reference.deficit = claimed - reference.usable;
        reference.uncommitted = 0;
    } else {
        reference.deficit = 0;
        reference.uncommitted = reference.usable - claimed;
    }
    return reference;
}

Snapshot compose_records(const std::vector<EvidenceRecord>& records, Capacity headroom) {
    EvidenceLedger ledger;
    fill(ledger, records);
    RackConfig config = linear_rack_config();
    config.headroom_floor = headroom;
    Result<ComposeOutcome> outcome = compose(ledger, config, compose_input(0));
    if (!outcome.has_value()) {
        RNF_REQUIRE(false);
    }
    return outcome->snapshot;
}

}  // namespace

RNF_TEST(differential, capacity_ledger_matches_reference_model) {
    // Device level only: the reference model deliberately ignores ports, links
    // and attachments so that the two implementations share nothing but the
    // definition of the ledger.
    const std::vector<std::vector<EvidenceRecord>> cases = {
        {},
        {member_record(1, 1), device_record(1, DeviceRole::kSwitch, Capacity{100},
                                            Availability::kUp)},
        {member_record(1, 1), device_record(1, DeviceRole::kSwitch, Capacity{100},
                                            Availability::kDown)},
        {member_record(1, 1), device_record(1, DeviceRole::kSwitch, Capacity{100},
                                            Availability::kUp, 0, kOperatorSource, 1, 
                                            Durability::kSticky, false)},
        {member_record(1, 1), member_record(2, 2),
         device_record(1, DeviceRole::kSwitch, Capacity{100}, Availability::kUp),
         device_record(2, DeviceRole::kHost, Capacity{50}, Availability::kDegraded)},
        {member_record(1, 1), member_record(2, 2),
         device_record(1, DeviceRole::kSwitch, Capacity{100}, Availability::kUp),
         device_record(2, DeviceRole::kHost, Capacity{50}, Availability::kUp),
         maintenance_record(MaintenanceKind::kDrain, ResourceKind::kDevice, 2, "x", 0,
                            kOperatorSource, 50)},
        {member_record(1, 1), device_record(1, DeviceRole::kSwitch, Capacity{100},
                                            Availability::kUp),
         obligation_record(1, ResourceKind::kRack, 0, Capacity{30}, PrincipalId{1}, "a")},
        {member_record(1, 1), device_record(1, DeviceRole::kSwitch, Capacity{100},
                                            Availability::kUp),
         obligation_record(1, ResourceKind::kRack, 0, Capacity{500}, PrincipalId{1}, "a")},
        {member_record(1, 1), member_record(2, 2),
         device_record(1, DeviceRole::kSwitch, Capacity{100}, Availability::kUp),
         device_record(2, DeviceRole::kHost, Capacity{50}, Availability::kUp),
         maintenance_record(MaintenanceKind::kMaintenance, ResourceKind::kRack, 0, "all", 0,
                            kOperatorSource, 60)},
        {member_record(1, 1, 0, kOperatorSource, 1),
         member_record(1, 2, 0, kDiscoverySource, 1),
         device_record(1, DeviceRole::kSwitch, Capacity{100}, Availability::kUp)},
    };
    const std::vector<Capacity> headrooms = {Capacity{0}, Capacity{10}, Capacity{1000}};
    for (const std::vector<EvidenceRecord>& records : cases) {
        for (Capacity headroom : headrooms) {
            const Snapshot snapshot = compose_records(records, headroom);
            const ReferenceCapacity reference =
                reference_capacity(records, snapshot->rack, headroom);
            RNF_CHECK_EQ(snapshot->capacity.total.units, reference.total);
            RNF_CHECK_EQ(snapshot->capacity.unavailable.units, reference.unavailable);
            RNF_CHECK_EQ(snapshot->capacity.usable.units, reference.usable);
            RNF_CHECK_EQ(snapshot->capacity.uncommitted.units, reference.uncommitted);
            RNF_CHECK_EQ(snapshot->capacity.deficit.units, reference.deficit);
            RNF_CHECK(snapshot->capacity.closes());
        }
    }
}

RNF_TEST(differential, exclusive_conflict_index_matches_pairwise_scan) {
    const std::vector<ResourceRef> scopes = {
        {ResourceKind::kRack, kRack.value()}, {ResourceKind::kDevice, 1},
        {ResourceKind::kDevice, 2},           {ResourceKind::kPort, 10},
        {ResourceKind::kPort, 11},            {ResourceKind::kPort, 12},
        {ResourceKind::kPort, 20},            {ResourceKind::kLink, 100},
        {ResourceKind::kLink, 101},           {ResourceKind::kAttachment, 200},
    };
    const std::vector<GrantMode> modes = {GrantMode::kShared, GrantMode::kExclusive};
    const Snapshot snapshot = [] {
        EvidenceLedger ledger;
        fill(ledger, linear_rack(0));
        Result<ComposeOutcome> outcome =
            compose(ledger, linear_rack_config(), compose_input(0));
        if (!outcome.has_value()) {
            RNF_REQUIRE(false);
        }
        return outcome->snapshot;
    }();

    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    std::uint64_t request_counter = 0;
    for (ResourceRef scope : scopes) {
        for (GrantMode mode : modes) {
            GrantRequest request;
            request.request.lo = ++request_counter;
            request.principal = PrincipalId{4};
            request.scope = scope;
            request.mode = mode;
            request.capacity = Capacity{0};
            request.ttl_ms = 60000;

            const Result<std::vector<GrantId>> predicted =
                registry.conflicts_bruteforce(snapshot, request);
            RNF_REQUIRE(predicted.has_value());
            const Result<Grant> actual =
                registry.acquire(snapshot, request, ControllerIncarnation{1}, 1000, true);
            if (predicted->empty()) {
                if (!actual.has_value()) {
                    ::rnf::test::abort_test(__FILE__, __LINE__,
                                            "the index reported no conflict for " +
                                                scope.to_text() + " but acquire failed with " +
                                                actual.error().describe());
                }
            } else {
                RNF_REQUIRE(!actual.has_value());
                RNF_CHECK(actual.error().code() == StatusCode::kExclusiveConflict);
            }
        }
    }
}

RNF_TEST(differential, scope_expansion_matches_naive_walk) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    Result<ComposeOutcome> outcome = compose(ledger, linear_rack_config(), compose_input(0));
    RNF_REQUIRE(outcome.has_value());
    const Snapshot snapshot = outcome->snapshot;

    // Naive reference: a resource belongs to a device scope when the device is
    // the resource itself, owns it, or terminates a link on it.
    auto owned_by = [&](ResourceRef ref, std::uint64_t device) {
        auto owners_of_port = [&](PortId port) {
            std::vector<std::uint64_t> owners;
            const PortRecord* record = snapshot.find_port(port);
            if (record != nullptr) {
                owners.push_back(record->device.value());
            }
            return owners;
        };
        switch (ref.kind) {
            case ResourceKind::kDevice:
                return ref.id == device;
            case ResourceKind::kPort: {
                const PortRecord* record = snapshot.find_port(PortId(ref.id));
                return record != nullptr && record->device.value() == device;
            }
            case ResourceKind::kLink: {
                const LinkRecord* record = snapshot.find_link(LinkId(ref.id));
                if (record == nullptr) {
                    return false;
                }
                const auto a = owners_of_port(record->port_a);
                const auto b = owners_of_port(record->port_b);
                return std::find(a.begin(), a.end(), device) != a.end() ||
                       std::find(b.begin(), b.end(), device) != b.end();
            }
            case ResourceKind::kAttachment: {
                const AttachmentRecord* record = snapshot.find_attachment(AttachmentId(ref.id));
                return record != nullptr && record->device.value() == device;
            }
            default:
                return false;
        }
    };

    for (std::uint64_t device : {1ULL, 2ULL}) {
        RNF_REQUIRE_VALUE(expanded,
                          expand_scope(snapshot, ResourceRef{ResourceKind::kDevice, device}));
        std::vector<ResourceRef> expected;
        for (const ResourceState& state : snapshot->resources) {
            if (state.ref.kind == ResourceKind::kRack) {
                continue;
            }
            if (owned_by(state.ref, device)) {
                expected.push_back(state.ref);
            }
        }
        std::sort(expected.begin(), expected.end());
        RNF_CHECK(expanded == expected);
    }
}
