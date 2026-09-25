// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Authority tests: grant binding, exclusivity, capacity accounting, fencing on
// epoch and member changes, lifecycle gates and durable-commit ambiguity.

#include <algorithm>
#include <string>
#include <vector>

#include "rnf/authority/registry.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/harness.hpp"

using namespace rnf;
using namespace rnf::test;

namespace {

constexpr TimestampMs kNow = 1700000000000ULL;

Snapshot rack_snapshot(std::uint64_t generation = 0, std::uint64_t epoch = 0,
                       LifecycleState lifecycle = LifecycleState::kActive) {
    EvidenceLedger ledger;
    fill(ledger, linear_rack(generation));
    Result<ComposeOutcome> outcome =
        compose(ledger, linear_rack_config(), compose_input(generation, epoch, lifecycle));
    if (!outcome.has_value()) {
        RNF_REQUIRE(false);
    }
    return outcome->snapshot;
}

Snapshot snapshot_of(const std::vector<EvidenceRecord>& records, std::uint64_t generation = 0,
                     std::uint64_t epoch = 0) {
    EvidenceLedger ledger;
    fill(ledger, records);
    Result<ComposeOutcome> outcome =
        compose(ledger, linear_rack_config(), compose_input(generation, epoch));
    if (!outcome.has_value()) {
        RNF_REQUIRE(false);
    }
    return outcome->snapshot;
}

GrantRequest request_of(std::uint64_t request_lo, ResourceRef scope, GrantMode mode = GrantMode::kShared,
                        std::uint64_t capacity = 0, std::uint64_t ttl_ms = 60000) {
    GrantRequest request;
    request.request.lo = request_lo;
    request.principal = PrincipalId{9};
    request.scope = scope;
    request.mode = mode;
    request.capacity = Capacity{capacity};
    request.ttl_ms = ttl_ms;
    return request;
}

}  // namespace

RNF_TEST(authority, acquire_validate_release_roundtrip) {
    const Snapshot snapshot = rack_snapshot();
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{5});
    RNF_REQUIRE_VALUE(grant,
                      registry.acquire(snapshot, request_of(1, {ResourceKind::kDevice, 1}),
                                       ControllerIncarnation{5}, kNow, true));
    RNF_CHECK(grant.state == GrantState::kActive);
    RNF_CHECK_EQ(grant.incarnation.value, 5U);
    RNF_CHECK_EQ(grant.epoch.value, 0U);
    RNF_CHECK_EQ(grant.fence, 1U);
    RNF_CHECK_EQ(registry.live_count(), 1U);

    RNF_REQUIRE_VALUE(validated, registry.validate(snapshot, grant.token(), kNow + 1));
    RNF_CHECK(validated.id == grant.id);

    RNF_REQUIRE_OK(registry.release(grant.token(), kNow + 2));
    RNF_CHECK_EQ(registry.live_count(), 0U);
    RNF_REQUIRE_CODE(registry.validate(snapshot, grant.token(), kNow + 3).error(),
                     StatusCode::kRefused);
    // Releasing twice is idempotent.
    RNF_REQUIRE_OK(registry.release(grant.token(), kNow + 4));
}

RNF_TEST(authority, request_identity_makes_acquire_idempotent) {
    const Snapshot snapshot = rack_snapshot();
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    const GrantRequest request = request_of(7, {ResourceKind::kDevice, 1}, GrantMode::kShared, 10);
    RNF_REQUIRE_VALUE(first, registry.acquire(snapshot, request, ControllerIncarnation{1}, kNow, true));
    RNF_REQUIRE_VALUE(second,
                      registry.acquire(snapshot, request, ControllerIncarnation{1}, kNow + 5, true));
    RNF_CHECK(first.id == second.id);
    RNF_CHECK_EQ(first.fence, second.fence);
    RNF_CHECK_EQ(registry.live_count(), 1U);
    // A refused request is remembered too, so a replay gets the same refusal.
    const GrantRequest conflicting =
        request_of(8, {ResourceKind::kDevice, 1}, GrantMode::kExclusive, 0);
    RNF_REQUIRE_CODE(
        registry.acquire(snapshot, conflicting, ControllerIncarnation{1}, kNow, true).error(),
        StatusCode::kExclusiveConflict);
    RNF_REQUIRE_CODE(
        registry.acquire(snapshot, conflicting, ControllerIncarnation{1}, kNow + 9, true).error(),
        StatusCode::kExclusiveConflict);
}

RNF_TEST(authority, exclusive_grants_cannot_overlap) {
    const Snapshot snapshot = rack_snapshot();
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    // A shared grant on port 11 touches only port 11 and link 101.
    RNF_REQUIRE_VALUE(shared,
                      registry.acquire(snapshot, request_of(1, {ResourceKind::kPort, 11}),
                                       ControllerIncarnation{1}, kNow, true));
    RNF_CHECK(shared.mode == GrantMode::kShared);
    // Another shared grant on the very same port is fine.
    RNF_REQUIRE_VALUE(second_shared,
                      registry.acquire(snapshot, request_of(2, {ResourceKind::kPort, 11}),
                                       ControllerIncarnation{1}, kNow, true));
    RNF_CHECK(second_shared.id == shared.id ? false : true);
    // An exclusive grant on the same port conflicts with both.
    RNF_REQUIRE_CODE(registry.acquire(snapshot,
                                      request_of(3, {ResourceKind::kPort, 11},
                                                 GrantMode::kExclusive),
                                      ControllerIncarnation{1}, kNow, true)
                         .error(),
                     StatusCode::kExclusiveConflict);
    // Device 2 is disjoint from port 11, so it can be held exclusively.
    RNF_REQUIRE_VALUE(other,
                      registry.acquire(snapshot,
                                       request_of(4, {ResourceKind::kDevice, 2},
                                                  GrantMode::kExclusive),
                                       ControllerIncarnation{1}, kNow, true));
    RNF_CHECK(other.mode == GrantMode::kExclusive);
    // While device 2 is exclusively held, another request that reaches it is
    // refused even when it is shared.
    RNF_REQUIRE_CODE(registry.acquire(snapshot, request_of(5, {ResourceKind::kLink, 100}),
                                      ControllerIncarnation{1}, kNow, true)
                         .error(),
                     StatusCode::kExclusiveConflict);
    // A rack wide exclusive grant conflicts with everything.
    RNF_REQUIRE_CODE(
        registry.acquire(snapshot,
                         request_of(6, {ResourceKind::kRack, kRack.value()}, GrantMode::kExclusive),
                         ControllerIncarnation{1}, kNow, true)
            .error(),
        StatusCode::kExclusiveConflict);
    RNF_CHECK_EQ(registry.live_count(), 3U);
    RNF_REQUIRE_OK(registry.release(other.token(), kNow + 1));
    RNF_CHECK_EQ(registry.live_count(), 2U);
}

RNF_TEST(authority, capacity_is_consumed_from_every_pool) {
    const Snapshot snapshot = rack_snapshot();
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    // The access port 20 carries 200 units and its device 500.
    RNF_REQUIRE_VALUE(grant,
                      registry.acquire(snapshot,
                                       request_of(1, {ResourceKind::kPort, 20}, GrantMode::kShared,
                                                  150),
                                       ControllerIncarnation{1}, kNow, true));
    RNF_REQUIRE_VALUE(port_free, registry.available(snapshot, {ResourceKind::kPort, 20}));
    RNF_CHECK_EQ(port_free.units, 50U);
    RNF_REQUIRE_VALUE(device_free, registry.available(snapshot, {ResourceKind::kDevice, 2}));
    RNF_CHECK_EQ(device_free.units, 350U);
    RNF_REQUIRE_VALUE(rack_free,
                      registry.available(snapshot, {ResourceKind::kRack, kRack.value()}));
    RNF_CHECK_EQ(rack_free.units, 1350U);

    // Asking for more than the port can carry is refused, not partially met.
    RNF_REQUIRE_CODE(registry.acquire(snapshot,
                                      request_of(2, {ResourceKind::kPort, 20}, GrantMode::kShared,
                                                 51),
                                      ControllerIncarnation{1}, kNow, true)
                         .error(),
                     StatusCode::kCapacityExhausted);
    RNF_REQUIRE_VALUE(fitting,
                      registry.acquire(snapshot,
                                       request_of(3, {ResourceKind::kPort, 20}, GrantMode::kShared,
                                                  50),
                                       ControllerIncarnation{1}, kNow, true));
    RNF_REQUIRE_VALUE(exhausted, registry.available(snapshot, {ResourceKind::kPort, 20}));
    RNF_CHECK_EQ(exhausted.units, 0U);
    // Releasing the larger grant returns exactly its own capacity, and the
    // smaller grant keeps holding its share.
    RNF_REQUIRE_OK(registry.release(grant.token(), kNow + 1));
    RNF_REQUIRE_VALUE(freed, registry.available(snapshot, {ResourceKind::kPort, 20}));
    RNF_CHECK_EQ(freed.units, 150U);
    RNF_REQUIRE_OK(registry.release(fitting.token(), kNow + 2));
    RNF_REQUIRE_VALUE(fully_freed, registry.available(snapshot, {ResourceKind::kPort, 20}));
    RNF_CHECK_EQ(fully_freed.units, 200U);
}

RNF_TEST(authority, out_of_rack_scope_is_refused) {
    const Snapshot snapshot = rack_snapshot();
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    RNF_REQUIRE_CODE(registry.acquire(snapshot, request_of(1, {ResourceKind::kDevice, 4242}),
                                      ControllerIncarnation{1}, kNow, true)
                         .error(),
                     StatusCode::kNotAMember);
    RNF_REQUIRE_CODE(registry.acquire(snapshot, request_of(2, {ResourceKind::kRack, 999}),
                                      ControllerIncarnation{1}, kNow, true)
                         .error(),
                     StatusCode::kOutOfRack);
}

RNF_TEST(authority, maintenance_blocks_use_without_fencing) {
    std::vector<EvidenceRecord> records = linear_rack(0);
    records.push_back(maintenance_record(MaintenanceKind::kDrain, ResourceKind::kDevice, 2, "work",
                                         0, kOperatorSource, 900));
    const Snapshot drained = snapshot_of(records);
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    RNF_REQUIRE_CODE(registry.acquire(drained, request_of(1, {ResourceKind::kDevice, 2}),
                                      ControllerIncarnation{1}, kNow, true)
                         .error(),
                     StatusCode::kScopeNotEligible);

    const Snapshot healthy = rack_snapshot();
    RNF_REQUIRE_VALUE(grant, registry.acquire(healthy, request_of(2, {ResourceKind::kDevice, 2}),
                                              ControllerIncarnation{1}, kNow, true));
    // The same lease presented against the drained snapshot is refused, but the
    // grant itself is not fenced: lifting the drain restores it.
    RNF_REQUIRE_CODE(registry.validate(drained, grant.token(), kNow + 1).error(),
                     StatusCode::kScopeNotEligible);
    RNF_REQUIRE_VALUE(restored, registry.validate(healthy, grant.token(), kNow + 2));
    RNF_CHECK(restored.id == grant.id);
}

RNF_TEST(authority, epoch_change_fences_the_lease) {
    const Snapshot epoch_zero = rack_snapshot(0, 0);
    std::vector<EvidenceRecord> records = linear_rack(0);
    records.push_back(member_record(3, 33, 1, kOperatorSource, 900));
    records.push_back(device_record(3, DeviceRole::kHost, Capacity{100}, Availability::kUp, 1,
                                    kOperatorSource, 901));
    const Snapshot epoch_one = snapshot_of(records, 1, 1);
    RNF_REQUIRE(!(epoch_zero->member_set_digest == epoch_one->member_set_digest));

    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    RNF_REQUIRE_VALUE(grant, registry.acquire(epoch_zero, request_of(1, {ResourceKind::kDevice, 1}),
                                              ControllerIncarnation{1}, kNow, true));
    RNF_REQUIRE_CODE(registry.validate(epoch_one, grant.token(), kNow + 1).error(),
                     StatusCode::kStaleEpoch);
}

RNF_TEST(authority, member_reincarnation_fences_the_lease) {
    std::vector<EvidenceRecord> first_generation = linear_rack(0);
    const Snapshot before = snapshot_of(first_generation);
    std::vector<EvidenceRecord> second_generation = linear_rack(0);
    // Device 1 leaves and rejoins under a new incarnation.
    second_generation.push_back(member_record(1, 1111, 1, kOperatorSource, 500));
    const Snapshot after = snapshot_of(second_generation, 1, 0);

    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    RNF_REQUIRE_VALUE(grant, registry.acquire(before, request_of(1, {ResourceKind::kDevice, 1}),
                                              ControllerIncarnation{1}, kNow, true));
    RNF_REQUIRE_CODE(registry.validate(after, grant.token(), kNow + 1).error(),
                     StatusCode::kFencedIncarnation);
}

RNF_TEST(authority, capacity_change_fences_the_lease) {
    const Snapshot before = snapshot_of(linear_rack(0));
    std::vector<EvidenceRecord> records = linear_rack(0);
    records.push_back(link_record(100, 10, 20, Capacity{100}, Availability::kUp, 0, kOperatorSource,
                                  700));
    const Snapshot after = snapshot_of(records);
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    RNF_REQUIRE_VALUE(grant, registry.acquire(before, request_of(1, {ResourceKind::kLink, 100}),
                                              ControllerIncarnation{1}, kNow, true));
    RNF_REQUIRE_CODE(registry.validate(after, grant.token(), kNow + 1).error(),
                     StatusCode::kFencedIncarnation);
}

RNF_TEST(authority, stale_incarnation_token_is_refused) {
    const Snapshot snapshot = rack_snapshot();
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    RNF_REQUIRE_VALUE(grant, registry.acquire(snapshot, request_of(1, {ResourceKind::kDevice, 1}),
                                              ControllerIncarnation{1}, kNow, true));
    LeaseToken forged = grant.token();
    forged.incarnation = ControllerIncarnation{2};
    RNF_REQUIRE_CODE(registry.validate(snapshot, forged, kNow + 1).error(),
                     StatusCode::kFencedIncarnation);
    LeaseToken wrong_fence = grant.token();
    wrong_fence.fence += 1;
    RNF_REQUIRE_CODE(registry.validate(snapshot, wrong_fence, kNow + 1).error(),
                     StatusCode::kFencedIncarnation);
    Digest mutated = grant.authority_basis;
    mutated.bytes[0] = static_cast<std::uint8_t>(mutated.bytes[0] ^ 0xFFU);
    LeaseToken wrong_basis = grant.token();
    wrong_basis.scope_basis = mutated;
    RNF_REQUIRE_CODE(registry.validate(snapshot, wrong_basis, kNow + 1).error(),
                     StatusCode::kFencedIncarnation);
}

RNF_TEST(authority, expiry_frees_capacity_once) {
    const Snapshot snapshot = rack_snapshot();
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    RNF_REQUIRE_VALUE(grant,
                      registry.acquire(snapshot,
                                       request_of(1, {ResourceKind::kPort, 20}, GrantMode::kShared,
                                                  200, 1000),
                                       ControllerIncarnation{1}, kNow, true));
    RNF_REQUIRE_VALUE(used, registry.available(snapshot, {ResourceKind::kPort, 20}));
    RNF_CHECK_EQ(used.units, 0U);
    RNF_CHECK(registry.expire_due(kNow + 10).empty());
    const std::vector<GrantId> expired = registry.expire_due(kNow + 1001);
    RNF_CHECK_EQ(expired.size(), 1U);
    RNF_CHECK(expired.front() == grant.id);
    RNF_CHECK(registry.expire_due(kNow + 2000).empty());
    RNF_REQUIRE_VALUE(freed, registry.available(snapshot, {ResourceKind::kPort, 20}));
    RNF_CHECK_EQ(freed.units, 200U);
    RNF_CHECK_EQ(registry.live_count(), 0U);
}

RNF_TEST(authority, recovery_fences_until_revalidated) {
    const Snapshot snapshot = rack_snapshot();
    GrantRegistry issuing;
    issuing.set_incarnation(ControllerIncarnation{1});
    RNF_REQUIRE_VALUE(grant, issuing.acquire(snapshot, request_of(1, {ResourceKind::kDevice, 1},
                                                                  GrantMode::kShared, 100),
                                             ControllerIncarnation{1}, kNow, true));

    GrantRegistry recovered;
    recovered.set_incarnation(ControllerIncarnation{2});
    RNF_REQUIRE_OK(recovered.install_recovered(snapshot, grant));
    RNF_CHECK_EQ(recovered.recovering_count(), 1U);
    RNF_CHECK_EQ(recovered.live_count(), 1U);
    RNF_REQUIRE_VALUE(reserved, recovered.available(snapshot, {ResourceKind::kDevice, 1}));
    RNF_CHECK_EQ(reserved.units, 900U);
    RNF_REQUIRE_CODE(recovered.validate(snapshot, grant.token(), kNow + 1).error(),
                     StatusCode::kIndeterminate);

    RNF_REQUIRE_VALUE(rearmed, recovered.revalidate(snapshot, ControllerIncarnation{2}, kNow + 2));
    RNF_CHECK_EQ(rearmed, 1U);
    RNF_CHECK_EQ(recovered.recovering_count(), 0U);
    RNF_REQUIRE_CODE(recovered.validate(snapshot, grant.token(), kNow + 3).error(),
                     StatusCode::kFencedIncarnation);
    const Grant* now = recovered.find(grant.id);
    RNF_REQUIRE(now != nullptr);
    RNF_CHECK_EQ(now->incarnation.value, 2U);
    RNF_CHECK(now->fence > grant.fence);
    RNF_REQUIRE_VALUE(validated, recovered.validate(snapshot, now->token(), kNow + 4));
    RNF_CHECK(validated.id == grant.id);
}

RNF_TEST(authority, recovery_of_a_changed_rack_fences_the_grant) {
    const Snapshot before = snapshot_of(linear_rack(0));
    GrantRegistry issuing;
    issuing.set_incarnation(ControllerIncarnation{1});
    RNF_REQUIRE_VALUE(grant, issuing.acquire(before, request_of(1, {ResourceKind::kDevice, 1}),
                                             ControllerIncarnation{1}, kNow, true));

    std::vector<EvidenceRecord> records = linear_rack(1);
    records.push_back(member_record(1, 777, 1, kOperatorSource, 800));
    const Snapshot after = snapshot_of(records, 1, 0);

    GrantRegistry recovered;
    recovered.set_incarnation(ControllerIncarnation{2});
    RNF_REQUIRE_OK(recovered.install_recovered(after, grant));
    RNF_REQUIRE_VALUE(rearmed, recovered.revalidate(after, ControllerIncarnation{2}, kNow + 1));
    RNF_CHECK_EQ(rearmed, 0U);
    const Grant* now = recovered.find(grant.id);
    RNF_REQUIRE(now != nullptr);
    RNF_CHECK(now->state == GrantState::kFenced);
    RNF_REQUIRE_VALUE(freed, recovered.available(after, {ResourceKind::kDevice, 1}));
    RNF_CHECK_EQ(freed.units, 1000U);
}

RNF_TEST(authority, suspend_and_resume_follow_the_lifecycle) {
    const Snapshot active = rack_snapshot();
    const Snapshot maintenance = rack_snapshot(0, 0, LifecycleState::kMaintenance);
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    RNF_REQUIRE_VALUE(grant, registry.acquire(active, request_of(1, {ResourceKind::kDevice, 1}),
                                              ControllerIncarnation{1}, kNow, true));
    registry.suspend_all("maintenance window");
    RNF_REQUIRE_CODE(registry.validate(active, grant.token(), kNow + 1).error(),
                     StatusCode::kLifecycleRefused);
    RNF_CHECK_EQ(registry.resume_all(active, kNow + 2), 1U);
    RNF_REQUIRE_VALUE(restored, registry.validate(active, grant.token(), kNow + 3));
    RNF_CHECK(restored.state == GrantState::kActive);

    // A suspended grant is re-armed when its scope is still sound; what the
    // lifecycle forbids is using it, not holding it.
    registry.suspend_all("again");
    RNF_CHECK_EQ(registry.resume_all(maintenance, kNow + 4), 1U);
    RNF_REQUIRE_CODE(registry.validate(maintenance, grant.token(), kNow + 5).error(),
                     StatusCode::kLifecycleRefused);
    RNF_REQUIRE_VALUE(back, registry.validate(active, grant.token(), kNow + 6));
    RNF_CHECK(back.state == GrantState::kActive);
}

RNF_TEST(authority, fence_all_releases_every_pool) {
    const Snapshot snapshot = rack_snapshot();
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    for (std::uint64_t i = 1; i <= 5; ++i) {
        RNF_REQUIRE_VALUE(grant,
                          registry.acquire(snapshot,
                                           request_of(i, {ResourceKind::kDevice, 1},
                                                      GrantMode::kShared, 100),
                                           ControllerIncarnation{1}, kNow, true));
        (void)grant;
    }
    RNF_CHECK_EQ(registry.live_count(), 5U);
    RNF_REQUIRE_VALUE(before, registry.available(snapshot, {ResourceKind::kDevice, 1}));
    RNF_CHECK_EQ(before.units, 500U);
    registry.fence_all("test");
    RNF_CHECK_EQ(registry.live_count(), 0U);
    RNF_REQUIRE_VALUE(after, registry.available(snapshot, {ResourceKind::kDevice, 1}));
    RNF_CHECK_EQ(after.units, 1000U);
    RNF_CHECK_EQ(registry.committed({ResourceKind::kDevice, 1}).value().units, 0U);
    RNF_CHECK_EQ(registry.occupancy_entries(), 0U);
    RNF_CHECK_EQ(registry.prune(), 0U);
}

RNF_TEST(authority, fence_device_touches_only_its_own_grants) {
    const Snapshot snapshot = rack_snapshot();
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    // Port 11 belongs to device 1 and reaches only link 101, so it is disjoint
    // from everything that touches device 2.
    RNF_REQUIRE_VALUE(on_one, registry.acquire(snapshot, request_of(1, {ResourceKind::kPort, 11}),
                                               ControllerIncarnation{1}, kNow, true));
    RNF_REQUIRE_VALUE(on_two, registry.acquire(snapshot, request_of(2, {ResourceKind::kDevice, 2}),
                                               ControllerIncarnation{1}, kNow, true));
    registry.fence_device(snapshot, DeviceId{2}, "device two retired");
    RNF_CHECK(registry.find(on_one.id) != nullptr);
    RNF_CHECK(registry.find(on_one.id)->state == GrantState::kActive);
    RNF_CHECK(registry.find(on_two.id)->state == GrantState::kFenced);
    RNF_CHECK_EQ(registry.live_count(), 1U);
}

RNF_TEST(authority, invalid_requests_are_refused) {
    const Snapshot snapshot = rack_snapshot();
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    GrantRequest no_request = request_of(0, {ResourceKind::kDevice, 1});
    RNF_REQUIRE_CODE(registry.acquire(snapshot, no_request, ControllerIncarnation{1}, kNow, true)
                         .error(),
                     StatusCode::kInvalidIdentity);
    GrantRequest no_principal = request_of(1, {ResourceKind::kDevice, 1});
    no_principal.principal = PrincipalId{0};
    RNF_REQUIRE_CODE(registry.acquire(snapshot, no_principal, ControllerIncarnation{1}, kNow, true)
                         .error(),
                     StatusCode::kInvalidIdentity);
    GrantRequest bad_ttl = request_of(2, {ResourceKind::kDevice, 1});
    bad_ttl.ttl_ms = 0;
    RNF_REQUIRE_CODE(registry.acquire(snapshot, bad_ttl, ControllerIncarnation{1}, kNow, true)
                         .error(),
                     StatusCode::kOutOfRange);
    GrantRequest huge_ttl = request_of(3, {ResourceKind::kDevice, 1});
    huge_ttl.ttl_ms = 10ULL * 365ULL * 24ULL * 60ULL * 60ULL * 1000ULL;
    RNF_REQUIRE_CODE(registry.acquire(snapshot, huge_ttl, ControllerIncarnation{1}, kNow, true)
                         .error(),
                     StatusCode::kOutOfRange);
    // The lifecycle gate refuses new authority without touching the registry.
    RNF_REQUIRE_CODE(registry.acquire(snapshot, request_of(4, {ResourceKind::kDevice, 1}),
                                      ControllerIncarnation{1}, kNow, false)
                         .error(),
                     StatusCode::kLifecycleRefused);
    RNF_CHECK_EQ(registry.live_count(), 0U);
}

RNF_TEST(authority, renew_extends_and_requires_a_live_lease) {
    const Snapshot snapshot = rack_snapshot();
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    RNF_REQUIRE_VALUE(grant,
                      registry.acquire(snapshot, request_of(1, {ResourceKind::kDevice, 1},
                                                            GrantMode::kShared, 0, 1000),
                                       ControllerIncarnation{1}, kNow, true));
    RNF_REQUIRE_VALUE(renewed, registry.renew(snapshot, grant.token(), 5000, kNow + 500));
    RNF_CHECK_EQ(renewed.expires_at_ms, kNow + 5500U);
    RNF_REQUIRE_CODE(registry.renew(snapshot, grant.token(), 0, kNow).error(),
                     StatusCode::kOutOfRange);
    LeaseToken stale = grant.token();
    stale.incarnation = ControllerIncarnation{99};
    RNF_REQUIRE_CODE(registry.renew(snapshot, stale, 100, kNow).error(),
                     StatusCode::kFencedIncarnation);
}

RNF_TEST(authority, registry_bounds_are_enforced) {
    const Snapshot snapshot = rack_snapshot();

    // Live grant bound.
    {
        AuthorityLimits limits;
        limits.max_live_grants = 3;
        GrantRegistry registry(limits);
        registry.set_incarnation(ControllerIncarnation{1});
        for (std::uint64_t i = 1; i <= 3; ++i) {
            RNF_REQUIRE_VALUE(grant,
                              registry.acquire(snapshot,
                                               request_of(i, {ResourceKind::kPort, 11}),
                                               ControllerIncarnation{1}, kNow, true));
            (void)grant;
        }
        RNF_REQUIRE_CODE(registry.acquire(snapshot, request_of(4, {ResourceKind::kPort, 11}),
                                          ControllerIncarnation{1}, kNow, true)
                             .error(),
                         StatusCode::kResourceExhausted);
    }

    // Remembered request bound. A refusal that is remembered still counts, so
    // the bound is checked before the next request is served.
    {
        AuthorityLimits limits;
        limits.max_remembered_requests = 4;
        GrantRegistry registry(limits);
        registry.set_incarnation(ControllerIncarnation{1});
        for (std::uint64_t i = 1; i <= 4; ++i) {
            RNF_REQUIRE_VALUE(grant,
                              registry.acquire(snapshot,
                                               request_of(i, {ResourceKind::kPort, 11}),
                                               ControllerIncarnation{1}, kNow, true));
            (void)grant;
        }
        RNF_REQUIRE_CODE(registry.acquire(snapshot, request_of(5, {ResourceKind::kPort, 11}),
                                          ControllerIncarnation{1}, kNow, true)
                             .error(),
                         StatusCode::kResourceExhausted);
    }

    // Exclusivity index bound: a rack scope would consume one entry per
    // resource, so a tiny budget refuses it rather than indexing half a scope.
    {
        AuthorityLimits limits;
        limits.max_occupancy_entries = 4;
        GrantRegistry registry(limits);
        registry.set_incarnation(ControllerIncarnation{1});
        RNF_REQUIRE_CODE(registry.acquire(snapshot,
                                          request_of(1, {ResourceKind::kRack, kRack.value()}),
                                          ControllerIncarnation{1}, kNow, true)
                             .error(),
                         StatusCode::kResourceExhausted);
        RNF_CHECK_EQ(registry.occupancy_entries(), 0U);
        RNF_CHECK_EQ(registry.live_count(), 0U);
    }

    // A bound that is reached must not corrupt the accounting: releasing a
    // grant returns exactly the capacity it took.
    {
        AuthorityLimits limits;
        limits.max_live_grants = 2;
        GrantRegistry registry(limits);
        registry.set_incarnation(ControllerIncarnation{1});
        RNF_REQUIRE_VALUE(first, registry.acquire(snapshot, request_of(1, {ResourceKind::kPort, 20},
                                                                      GrantMode::kShared, 100),
                                                  ControllerIncarnation{1}, kNow, true));
        RNF_REQUIRE_VALUE(second, registry.acquire(snapshot, request_of(2, {ResourceKind::kPort, 20},
                                                                       GrantMode::kShared, 50),
                                                   ControllerIncarnation{1}, kNow, true));
        RNF_REQUIRE_CODE(registry.acquire(snapshot, request_of(3, {ResourceKind::kPort, 20},
                                                               GrantMode::kShared, 1),
                                          ControllerIncarnation{1}, kNow, true)
                             .error(),
                         StatusCode::kResourceExhausted);
        RNF_REQUIRE_OK(registry.release(first.token(), kNow + 1));
        RNF_REQUIRE_VALUE(after, registry.available(snapshot, {ResourceKind::kPort, 20}));
        RNF_CHECK_EQ(after.units, 150U);
        RNF_REQUIRE_OK(registry.release(second.token(), kNow + 2));
        RNF_REQUIRE_VALUE(empty, registry.available(snapshot, {ResourceKind::kPort, 20}));
        RNF_CHECK_EQ(empty.units, 200U);
        RNF_CHECK_EQ(registry.occupancy_entries(), 0U);
    }
}

RNF_TEST(authority, lookup_reports_unknown_for_unseen_requests) {
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    RequestId unseen;
    unseen.lo = 12345;
    RNF_REQUIRE_CODE(registry.lookup_request(unseen).error(), StatusCode::kUnknown);

    const Snapshot snapshot = rack_snapshot();
    RNF_REQUIRE_VALUE(grant, registry.acquire(snapshot, request_of(77, {ResourceKind::kDevice, 1}),
                                              ControllerIncarnation{1}, kNow, true));
    RequestId seen;
    seen.lo = 77;
    RNF_REQUIRE_VALUE(outcome, registry.lookup_request(seen));
    RNF_CHECK(outcome.code == StatusCode::kOk);
    RNF_CHECK(outcome.grant == grant.id);
    RNF_CHECK_EQ(outcome.fence, grant.fence);
}

RNF_TEST(authority, grants_bind_the_generation_they_were_issued_at) {
    const Snapshot generation_zero = rack_snapshot(0, 0);
    GrantRegistry registry;
    registry.set_incarnation(ControllerIncarnation{1});
    RNF_REQUIRE_VALUE(grant, registry.acquire(generation_zero, request_of(1, {ResourceKind::kDevice, 1}),
                                              ControllerIncarnation{1}, kNow, true));
    RNF_CHECK_EQ(grant.generation.value, 0U);

    // A later generation with the same members and capacities keeps the lease.
    const Snapshot generation_five = rack_snapshot(5, 0);
    RNF_REQUIRE_VALUE(validated, registry.validate(generation_five, grant.token(), kNow + 1));
    RNF_CHECK_EQ(validated.generation.value, 0U);

    // A generation older than the grant is refused outright.
    Grant future;
    future.id = GrantId(99);
    future.request.lo = 5;
    future.principal = PrincipalId{9};
    future.scope = ResourceRef{ResourceKind::kDevice, 1};
    future.generation = TopologyGeneration{9};
    future.epoch = generation_zero.epoch();
    future.incarnation = ControllerIncarnation{1};
    future.fence = 500;
    future.expires_at_ms = kNow + 10000;
    future.state = GrantState::kActive;
    RNF_REQUIRE_VALUE(future_scope, expand_scope(generation_zero, future.scope));
    future.authority_basis = authority_basis_digest(generation_zero, future_scope);
    GrantRegistry other;
    other.set_incarnation(ControllerIncarnation{2});
    RNF_REQUIRE_OK(other.install_recovered(generation_zero, future));
    RNF_REQUIRE_CODE(other.validate(generation_zero, future.token(), kNow + 1).error(),
                     StatusCode::kIndeterminate);
}
