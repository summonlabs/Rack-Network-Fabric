// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Independent consumer of the installed Rack Network Fabric package. It uses
// only the public headers and the exported RNF::rnf target, exercises the
// composition, authority and persistence APIs, and exits non-zero if any
// invariant it checks does not hold.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <rnf/authority/registry.hpp>
#include <rnf/compose/composer.hpp>
#include <rnf/model/evidence.hpp>
#include <rnf/persist/store.hpp>
#include <rnf/version.hpp>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

rnf::EvidenceRecord make_member(std::uint64_t device, std::uint64_t sequence) {
    rnf::MemberClaim claim;
    claim.rack = rnf::RackId(42);
    claim.device = rnf::DeviceId(device);
    claim.incarnation = device * 3;
    rnf::EvidenceRecord record;
    record.kind = rnf::EvidenceKind::kMember;
    record.durability = rnf::Durability::kSticky;
    record.provenance.source = rnf::SourceId(1);
    record.provenance.kind = rnf::SourceKind::kOperator;
    record.provenance.sequence = sequence;
    record.payload = claim;
    return record;
}

rnf::EvidenceRecord make_device(std::uint64_t device, std::uint64_t capacity,
                                std::uint64_t sequence) {
    rnf::DeviceClaim claim;
    claim.device = rnf::DeviceId(device);
    claim.role = rnf::DeviceRole::kSwitch;
    claim.capacity = rnf::Capacity{capacity};
    claim.availability = rnf::Availability::kUp;
    rnf::EvidenceRecord record;
    record.kind = rnf::EvidenceKind::kDevice;
    record.durability = rnf::Durability::kSticky;
    record.provenance.source = rnf::SourceId(1);
    record.provenance.kind = rnf::SourceKind::kOperator;
    record.provenance.sequence = sequence;
    record.payload = claim;
    return record;
}

}  // namespace

int main() {
    std::printf("linked against Rack Network Fabric %s\n",
                std::string(rnf::kVersionString).c_str());
    check(rnf::kVersionMajor == 1, "library major version is 1");

    rnf::EvidenceLedger ledger;
    (void)ledger.insert(make_member(1, 1));
    (void)ledger.insert(make_member(2, 2));
    (void)ledger.insert(make_device(1, 1000, 3));
    (void)ledger.insert(make_device(2, 500, 4));

    rnf::RackConfig config;
    config.rack = rnf::RackId(42);
    config.name = "downstream";
    config.headroom_floor = rnf::Capacity{100};

    rnf::ComposeInput input;
    input.generation = rnf::TopologyGeneration{1};
    input.epoch = rnf::RackEpoch{1};
    input.lifecycle = rnf::LifecycleState::kActive;

    rnf::Result<rnf::ComposeOutcome> composed = rnf::compose(ledger, config, input);
    if (!composed.has_value()) {
        std::printf("FAIL compose: %s\n", composed.error().describe().c_str());
        return 1;
    }
    const rnf::Snapshot snapshot = composed->snapshot;
    check(snapshot.valid(), "snapshot is valid");
    check(snapshot->capacity.total.units == 1500, "total capacity is 1500");
    check(snapshot->capacity.uncommitted.units == 1400, "uncommitted capacity is 1400");
    check(snapshot->capacity.closes(), "capacity ledger closes exactly");
    check(snapshot->devices.size() == 2, "two member devices");
    check(!snapshot.digest().is_zero(), "snapshot has a content address");

    // Equivalent evidence in a different order gives the same content address.
    rnf::EvidenceLedger shuffled;
    (void)shuffled.insert(make_device(2, 500, 4));
    (void)shuffled.insert(make_member(2, 2));
    (void)shuffled.insert(make_device(1, 1000, 3));
    (void)shuffled.insert(make_member(1, 1));
    rnf::Result<rnf::ComposeOutcome> again = rnf::compose(shuffled, config, input);
    check(again.has_value(), "second composition succeeds");
    if (again.has_value()) {
        check(again->snapshot.digest() == snapshot.digest(),
              "equivalent evidence yields the same content address");
    }

    rnf::GrantRegistry registry;
    registry.set_incarnation(rnf::ControllerIncarnation{1});
    rnf::GrantRequest request;
    request.request.lo = 1;
    request.principal = rnf::PrincipalId{7};
    request.scope = rnf::ResourceRef{rnf::ResourceKind::kDevice, 1};
    request.mode = rnf::GrantMode::kExclusive;
    request.capacity = rnf::Capacity{250};
    request.ttl_ms = 60000;

    const rnf::Result<rnf::Grant> granted =
        registry.acquire(snapshot, request, rnf::ControllerIncarnation{1}, 1700000000000ULL, true);
    check(granted.has_value(), "exclusive grant is issued");
    if (granted.has_value()) {
        check(registry.validate(snapshot, granted->token(), 1700000000001ULL).has_value(),
              "issued lease validates");
        const rnf::Result<rnf::Capacity> free =
            registry.available(snapshot, rnf::ResourceRef{rnf::ResourceKind::kDevice, 1});
        check(free.has_value() && free->units == 750, "capacity closed after the grant");
        check(registry.release(granted->token(), 1700000000002ULL).ok(), "lease released");
        check(registry.live_count() == 0, "no live grants remain");
    }

    // Persistence round trip through a real directory.
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "rnf-downstream-store";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    {
        rnf::Result<std::unique_ptr<rnf::Store>> opened =
            rnf::Store::open(directory, rnf::StoreOptions{});
        check(opened.has_value(), "store opens");
        if (opened.has_value()) {
            std::unique_ptr<rnf::Store> store = std::move(*opened);
            check(store->bind_rack(rnf::RackId(42)).ok(), "store binds to the rack");
            const rnf::Result<rnf::ControllerIncarnation> incarnation = store->next_incarnation();
            check(incarnation.has_value() && incarnation->value == 1,
                  "first incarnation is 1");
            rnf::StoreRecord record;
            record.type = rnf::RecordType::kEvidence;
            record.payload = make_device(1, 1000, 3);
            check(store->append(record).ok(), "record appended durably");
            check(store->mark_clean_shutdown(1).ok(), "store marked cleanly closed");
        }
    }
    {
        rnf::Result<std::unique_ptr<rnf::Store>> opened =
            rnf::Store::open(directory, rnf::StoreOptions{});
        check(opened.has_value(), "store reopens");
        if (opened.has_value()) {
            std::unique_ptr<rnf::Store> store = std::move(*opened);
            check(store->recovery().clean_shutdown, "previous shutdown was clean");
            check(store->recovered().records.size() == 1, "one record replayed");
            const rnf::Result<rnf::ControllerIncarnation> incarnation = store->next_incarnation();
            check(incarnation.has_value() && incarnation->value == 2,
                  "second incarnation is 2");
            check(store->mark_clean_shutdown(2).ok(), "store marked cleanly closed again");
        }
    }
    std::filesystem::remove_all(directory, ec);

    if (failures == 0) {
        std::printf("downstream consumer: all checks passed\n");
        return 0;
    }
    std::printf("downstream consumer: %d checks failed\n", failures);
    return 1;
}
