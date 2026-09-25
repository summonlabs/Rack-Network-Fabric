// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Embedding example: compose a rack from evidence, inspect what became
// authoritative, and take a lease - all in process, with no daemon.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "rnf/authority/registry.hpp"
#include "rnf/compose/composer.hpp"
#include "rnf/model/evidence.hpp"

namespace {

rnf::EvidenceRecord member(std::uint64_t device, std::uint64_t incarnation, std::uint64_t seq) {
    rnf::MemberClaim claim;
    claim.rack = rnf::RackId(7);
    claim.device = rnf::DeviceId(device);
    claim.incarnation = incarnation;
    rnf::EvidenceRecord record;
    record.kind = rnf::EvidenceKind::kMember;
    record.durability = rnf::Durability::kSticky;
    record.provenance.source = rnf::SourceId(1);
    record.provenance.kind = rnf::SourceKind::kOperator;
    record.provenance.sequence = seq;
    record.payload = claim;
    return record;
}

rnf::EvidenceRecord device(std::uint64_t id, rnf::Capacity capacity, std::uint64_t seq) {
    rnf::DeviceClaim claim;
    claim.device = rnf::DeviceId(id);
    claim.role = rnf::DeviceRole::kSwitch;
    claim.capacity = capacity;
    claim.availability = rnf::Availability::kUp;
    rnf::EvidenceRecord record;
    record.kind = rnf::EvidenceKind::kDevice;
    record.durability = rnf::Durability::kSticky;
    record.provenance.source = rnf::SourceId(1);
    record.provenance.kind = rnf::SourceKind::kOperator;
    record.provenance.sequence = seq;
    record.payload = claim;
    return record;
}

rnf::EvidenceRecord port(std::uint64_t id, std::uint64_t owner, std::uint16_t index,
                         rnf::PortRole role, rnf::Capacity capacity, std::uint64_t seq) {
    rnf::PortClaim claim;
    claim.port = rnf::PortId(id);
    claim.device = rnf::DeviceId(owner);
    claim.index = index;
    claim.role = role;
    claim.capacity = capacity;
    claim.availability = rnf::Availability::kUp;
    claim.admin = rnf::AdminState::kEnabled;
    rnf::EvidenceRecord record;
    record.kind = rnf::EvidenceKind::kPort;
    record.durability = rnf::Durability::kSticky;
    record.provenance.source = rnf::SourceId(1);
    record.provenance.kind = rnf::SourceKind::kOperator;
    record.provenance.sequence = seq;
    record.payload = claim;
    return record;
}

}  // namespace

int main() {
    rnf::EvidenceLedger ledger;
    (void)ledger.insert(member(1, 11, 1));
    (void)ledger.insert(member(2, 22, 2));
    (void)ledger.insert(device(1, rnf::Capacity{1000}, 3));
    (void)ledger.insert(device(2, rnf::Capacity{500}, 4));
    (void)ledger.insert(port(10, 1, 0, rnf::PortRole::kUplink, rnf::Capacity{400}, 5));
    (void)ledger.insert(port(20, 2, 0, rnf::PortRole::kAccess, rnf::Capacity{200}, 6));

    rnf::RackConfig config;
    config.rack = rnf::RackId(7);
    config.name = "example-rack";
    config.headroom_floor = rnf::Capacity{50};

    rnf::ComposeInput input;
    input.generation = rnf::TopologyGeneration{1};
    input.epoch = rnf::RackEpoch{1};
    input.lifecycle = rnf::LifecycleState::kActive;

    rnf::Result<rnf::ComposeOutcome> composed = rnf::compose(ledger, config, input);
    if (!composed.has_value()) {
        std::cerr << "compose failed: " << composed.error().describe() << "\n";
        return 1;
    }
    const rnf::Snapshot snapshot = composed->snapshot;

    std::cout << "snapshot digest   : " << snapshot.digest().to_hex() << "\n";
    std::cout << "generation/epoch  : " << snapshot.generation().value << "/"
              << snapshot.epoch().value << "\n";
    std::cout << "members           : " << snapshot->devices.size() << "\n";
    std::cout << "capacity total    : " << snapshot->capacity.total.units << "\n";
    std::cout << "capacity usable   : " << snapshot->capacity.usable.units << "\n";
    std::cout << "capacity obligated: " << snapshot->capacity.obligated.units << "\n";
    std::cout << "capacity headroom : " << snapshot->capacity.headroom_floor.units << "\n";
    std::cout << "capacity free     : " << snapshot->capacity.uncommitted.units << "\n";
    std::cout << "ledger closes     : " << (snapshot->capacity.closes() ? "yes" : "no") << "\n";
    for (const rnf::Diagnostic& diagnostic : snapshot->diagnostics) {
        std::cout << "diagnostic        : " << rnf::to_string(diagnostic.kind) << " subject "
                  << diagnostic.subject_id << " " << diagnostic.detail << "\n";
    }

    rnf::GrantRegistry registry;
    registry.set_incarnation(rnf::ControllerIncarnation{1});
    rnf::GrantRequest request;
    request.request.lo = 1;
    request.principal = rnf::PrincipalId{5};
    request.scope = rnf::ResourceRef{rnf::ResourceKind::kDevice, 1};
    request.mode = rnf::GrantMode::kShared;
    request.capacity = rnf::Capacity{200};
    request.ttl_ms = 60000;
    rnf::Result<rnf::Grant> granted = registry.acquire(snapshot, request,
                                                      rnf::ControllerIncarnation{1}, 1700000000000ULL,
                                                      true);
    if (!granted.has_value()) {
        std::cerr << "acquire failed: " << granted.error().describe() << "\n";
        return 1;
    }
    std::cout << "grant             : " << granted->id.to_hex() << " fence "
              << granted->fence << " basis " << granted->authority_basis.to_hex() << "\n";
    const rnf::Result<rnf::Capacity> free =
        registry.available(snapshot, rnf::ResourceRef{rnf::ResourceKind::kDevice, 1});
    if (free.has_value()) {
        std::cout << "device 1 free     : " << free->units << "\n";
    }
    (void)registry.release(granted->token(), 1700000000001ULL);
    std::cout << "live grants       : " << registry.live_count() << "\n";
    return 0;
}
