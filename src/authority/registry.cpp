// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/authority/registry.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "rnf/core/checked.hpp"
#include "rnf/core/hash.hpp"

namespace rnf {
namespace {

const AuthorityLimits kLimits{};

[[nodiscard]] bool is_live_state(GrantState state) noexcept {
    switch (state) {
        case GrantState::kPending:
        case GrantState::kRecovering:
        case GrantState::kActive:
        case GrantState::kSuspended:
            return true;
        case GrantState::kReleased:
        case GrantState::kExpired:
        case GrantState::kFenced:
            return false;
    }
    return false;
}

[[nodiscard]] Digest request_digest_of(const GrantRequest& request) {
    Blake2s256 hasher;
    hasher.update("rnf.request.v1");
    hasher.update_le64(request.request.hi);
    hasher.update_le64(request.request.lo);
    hasher.update_le64(request.principal.value());
    hasher.update_le64(static_cast<std::uint64_t>(request.scope.kind));
    hasher.update_le64(request.scope.id);
    hasher.update_le64(static_cast<std::uint64_t>(request.mode));
    hasher.update_le64(request.capacity.units);
    hasher.update_le64(request.ttl_ms);
    return hasher.final();
}

/// True when a live grant occupying a resource is incompatible with a request.
[[nodiscard]] bool occupant_conflicts(GrantMode requested, GrantMode held) noexcept {
    return requested == GrantMode::kExclusive || held == GrantMode::kExclusive;
}

}  // namespace

const AuthorityLimits& default_authority_limits() noexcept {
    return kLimits;
}

bool GrantRegistry::is_live(GrantState state) const noexcept {
    return is_live_state(state);
}

Result<std::vector<ResourceRef>> GrantRegistry::footprint(const Snapshot& snapshot,
                                                          ResourceRef root) const {
    return expand_scope(snapshot, root);
}

void GrantRegistry::attach(const Snapshot& snapshot, const Grant& grant) {
    const Result<std::vector<ResourceRef>> expanded = expand_scope(snapshot, grant.scope);
    if (expanded.has_value()) {
        std::vector<ResourceRef>& scope = footprints_[grant.id];
        scope = *expanded;
        for (ResourceRef ref : scope) {
            auto& occupants = occupancy_[ref];
            occupants.push_back(Occupant{grant.id, grant.mode});
            std::sort(occupants.begin(), occupants.end(),
                      [](const Occupant& a, const Occupant& b) { return a.id < b.id; });
            ++occupancy_entries_;
        }
    }
    for (ResourceRef ref : grant.capacity_pool) {
        std::uint64_t& total = committed_[ref];
        std::uint64_t next = 0;
        total = checked_add_u64(total, grant.capacity.units, next)
                    ? next
                    : std::numeric_limits<std::uint64_t>::max();
    }
    std::uint64_t next_total = 0;
    committed_total_.units =
        checked_add_u64(committed_total_.units, grant.capacity.units, next_total)
            ? next_total
            : std::numeric_limits<std::uint64_t>::max();
}

void GrantRegistry::detach(const Grant& grant) {
    const auto scope_it = footprints_.find(grant.id);
    if (scope_it != footprints_.end()) {
        for (ResourceRef ref : scope_it->second) {
            const auto occ_it = occupancy_.find(ref);
            if (occ_it == occupancy_.end()) {
                continue;
            }
            auto& occupants = occ_it->second;
            const auto removed =
                std::remove_if(occupants.begin(), occupants.end(), [&grant](const Occupant& o) {
                    return o.id == grant.id;
                });
            const std::size_t removed_count =
                static_cast<std::size_t>(std::distance(removed, occupants.end()));
            occupants.erase(removed, occupants.end());
            occupancy_entries_ -= removed_count;
            if (occupants.empty()) {
                occupancy_.erase(occ_it);
            }
        }
        footprints_.erase(scope_it);
    }
    for (ResourceRef ref : grant.capacity_pool) {
        const auto it = committed_.find(ref);
        if (it == committed_.end()) {
            continue;
        }
        std::uint64_t next = 0;
        if (checked_sub_u64(it->second, grant.capacity.units, next) && next > 0) {
            it->second = next;
        } else {
            committed_.erase(it);
        }
    }
    std::uint64_t next_total = 0;
    committed_total_.units =
        checked_sub_u64(committed_total_.units, grant.capacity.units, next_total) ? next_total : 0;
}

std::size_t GrantRegistry::prune() {
    std::vector<GrantId> stale;
    for (const auto& [id, scope] : footprints_) {
        (void)scope;
        const auto grant_it = grants_.find(id);
        if (grant_it == grants_.end() || !is_live(grant_it->second.state)) {
            stale.push_back(id);
        }
    }
    for (GrantId id : stale) {
        const Grant& grant = grants_.at(id);
        detach(grant);
    }
    return stale.size();
}

Status GrantRegistry::check_capacity(const Snapshot& snapshot,
                                     const std::vector<ResourceRef>& pool,
                                     Capacity requested) const {
    for (ResourceRef ref : pool) {
        const Result<Capacity> free_capacity = available(snapshot, ref);
        if (!free_capacity.has_value()) {
            const Status& failure = free_capacity.error();
            return Status(failure.code(),
                          failure.detail() + " (resource " + ref.to_text() + ")");
        }
        if (free_capacity->units < requested.units) {
            return Status(StatusCode::kCapacityExhausted,
                          "requested " + std::to_string(requested.units) + " units but only " +
                              std::to_string(free_capacity->units) + " are available in " +
                              ref.to_text());
        }
    }
    return Status{};
}

Result<Capacity> GrantRegistry::available(const Snapshot& snapshot, ResourceRef ref) const {
    if (!snapshot.valid()) {
        return Status(StatusCode::kInvalidArgument, "snapshot handle is empty");
    }
    Capacity base{};
    if (ref.kind == ResourceKind::kRack) {
        if (ref.id != snapshot.data().rack.value()) {
            return Status(StatusCode::kOutOfRack, "capacity query names a different rack");
        }
        const ResourceState* rack_state = snapshot.find(ref);
        if (rack_state == nullptr) {
            return Status(StatusCode::kNotFound, "rack resource is missing from the snapshot");
        }
        if (rack_state->eligibility != Eligibility::kEligible) {
            return Status(StatusCode::kScopeNotEligible, "the rack itself is not eligible");
        }
        base = snapshot.data().capacity.uncommitted;
    } else {
        const ResourceState* state = snapshot.find(ref);
        if (state == nullptr) {
            return Status(StatusCode::kNotAMember, "resource is not part of this rack");
        }
        if (!state->capacity_known) {
            return Status(StatusCode::kUnknown, "resource capacity has not been measured");
        }
        if (state->eligibility != Eligibility::kEligible) {
            return Status(StatusCode::kScopeNotEligible, "resource is not eligible");
        }
        std::uint64_t after_obligations = 0;
        if (!checked_sub_u64(state->capacity.units, state->obligated.units, after_obligations)) {
            return Status(StatusCode::kCapacityExhausted,
                          "imported obligations exceed the capacity of this resource");
        }
        base.units = after_obligations;
    }

    const auto it = committed_.find(ref);
    const std::uint64_t used = it == committed_.end() ? 0 : it->second;
    std::uint64_t remaining = 0;
    if (!checked_sub_u64(base.units, used, remaining)) {
        return Status(StatusCode::kCapacityExhausted,
                      "issued authority already exceeds the capacity of " + ref.to_text());
    }
    return Capacity{remaining};
}

Result<Capacity> GrantRegistry::committed(ResourceRef ref) const {
    const auto it = committed_.find(ref);
    return Capacity{it == committed_.end() ? 0 : it->second};
}

void GrantRegistry::remember(const GrantRequest& request, const RequestOutcome& outcome) {
    RequestOutcome stored = outcome;
    stored.request_digest = request_digest_of(request);
    outcomes_[request.request] = std::move(stored);
}

Result<Grant> GrantRegistry::acquire(const Snapshot& snapshot, const GrantRequest& request,
                                     ControllerIncarnation incarnation, TimestampMs now_ms,
                                     bool allow_new_authority) {
    if (!snapshot.valid()) {
        return Status(StatusCode::kInvalidArgument, "snapshot handle is empty");
    }
    if (!request.request.is_set()) {
        return Status(StatusCode::kInvalidIdentity, "grant request needs a request identity");
    }

    // Idempotency: a replayed request returns the original outcome and changes
    // nothing. This is what makes a lost acknowledgement safe.
    const auto remembered = outcomes_.find(request.request);
    if (remembered != outcomes_.end()) {
        const RequestOutcome& outcome = remembered->second;
        if (outcome.code != StatusCode::kOk) {
            return Status(outcome.code, outcome.detail);
        }
        const auto grant_it = grants_.find(outcome.grant);
        if (grant_it == grants_.end()) {
            return Status(StatusCode::kIndeterminate,
                          "the remembered outcome refers to a grant that is no longer present");
        }
        return grant_it->second;
    }

    if (!request.principal.is_set()) {
        return Status(StatusCode::kInvalidIdentity, "grant request needs a principal");
    }
    if (request.scope.id == 0 && request.scope.kind != ResourceKind::kRack) {
        return Status(StatusCode::kInvalidScope, "grant scope has no resource identity");
    }
    if (request.ttl_ms < limits_.min_ttl_ms || request.ttl_ms > limits_.max_ttl_ms) {
        return Status(StatusCode::kOutOfRange, "requested lease lifetime is out of range");
    }
    if (!allow_new_authority) {
        RequestOutcome outcome;
        outcome.code = StatusCode::kLifecycleRefused;
        outcome.detail = "the rack lifecycle does not accept new authority";
        remember(request, outcome);
        return Status(outcome.code, outcome.detail);
    }
    if (live_ >= limits_.max_live_grants) {
        return Status(StatusCode::kResourceExhausted, "live grant bound reached");
    }
    if (outcomes_.size() >= limits_.max_remembered_requests) {
        return Status(StatusCode::kResourceExhausted,
                      "remembered request bound reached; restart the daemon to clear it");
    }

    const Result<std::vector<ResourceRef>> expanded = footprint(snapshot, request.scope);
    if (!expanded.has_value()) {
        RequestOutcome outcome;
        outcome.code = expanded.error().code();
        outcome.detail = expanded.error().detail();
        remember(request, outcome);
        return expanded.error();
    }
    if (occupancy_entries_ + expanded->size() > limits_.max_occupancy_entries) {
        return Status(StatusCode::kResourceExhausted,
                      "exclusivity index bound reached for this scope");
    }

    for (ResourceRef ref : *expanded) {
        const ResourceState* state = snapshot.find(ref);
        if (state == nullptr) {
            return Status(StatusCode::kNotFound, "scope names a resource outside this rack");
        }
        if (state->eligibility != Eligibility::kEligible) {
            RequestOutcome outcome;
            outcome.code = StatusCode::kScopeNotEligible;
            outcome.detail = "scope contains a resource that is not eligible: " + ref.to_text() +
                             " (" + std::string(to_string(state->eligibility)) + ")";
            remember(request, outcome);
            return Status(outcome.code, outcome.detail);
        }
    }

    for (ResourceRef ref : *expanded) {
        const auto occ_it = occupancy_.find(ref);
        if (occ_it == occupancy_.end()) {
            continue;
        }
        for (const Occupant& occupant : occ_it->second) {
            const auto grant_it = grants_.find(occupant.id);
            if (grant_it == grants_.end() || !is_live(grant_it->second.state)) {
                continue;
            }
            if (occupant_conflicts(request.mode, occupant.mode)) {
                RequestOutcome outcome;
                outcome.code = StatusCode::kExclusiveConflict;
                outcome.detail = "conflicts with " + std::string(to_string(occupant.mode)) +
                                 " grant " + occupant.id.to_hex() + " on " + ref.to_text();
                remember(request, outcome);
                return Status(outcome.code, outcome.detail);
            }
        }
    }

    const Result<std::vector<ResourceRef>> pool = capacity_pool_refs(snapshot, request.scope);
    if (!pool.has_value()) {
        return pool.error();
    }
    const Status capacity_status = check_capacity(snapshot, *pool, request.capacity);
    if (!capacity_status.ok()) {
        RequestOutcome outcome;
        outcome.code = capacity_status.code();
        outcome.detail = capacity_status.detail();
        remember(request, outcome);
        return capacity_status;
    }

    Grant grant;
    grant.id = GrantId(++next_fence_);
    grant.request = request.request;
    grant.principal = request.principal;
    grant.scope = request.scope;
    grant.mode = request.mode;
    grant.capacity = request.capacity;
    grant.generation = snapshot.generation();
    grant.epoch = snapshot.epoch();
    grant.incarnation = incarnation;
    grant.fence = next_fence_;
    grant.issued_at_ms = now_ms;
    std::uint64_t deadline = 0;
    if (!checked_add_u64(now_ms, request.ttl_ms, deadline)) {
        return Status(StatusCode::kOutOfRange, "lease deadline overflowed");
    }
    grant.expires_at_ms = deadline;
    grant.state = GrantState::kActive;
    grant.authority_basis = authority_basis_digest(snapshot, *expanded);
    grant.capacity_pool = *pool;

    attach(snapshot, grant);
    grants_[grant.id] = grant;
    ++live_;

    RequestOutcome outcome;
    outcome.code = StatusCode::kOk;
    outcome.grant = grant.id;
    outcome.fence = grant.fence;
    outcome.incarnation = grant.incarnation;
    remember(request, outcome);
    return grant;
}

Result<Grant> GrantRegistry::check_lease(const Snapshot& snapshot, const LeaseToken& token,
                                         TimestampMs now_ms) const {
    if (!snapshot.valid()) {
        return Status(StatusCode::kInvalidArgument, "snapshot handle is empty");
    }
    const auto grant_it = grants_.find(token.grant);
    if (grant_it == grants_.end()) {
        return Status(StatusCode::kNotFound, "no such grant");
    }
    const Grant& grant = grant_it->second;

    if (grant.fence != token.fence || !(grant.authority_basis == token.scope_basis)) {
        return Status(StatusCode::kFencedIncarnation, "lease token does not match the grant record");
    }
    if (!(grant.incarnation == token.incarnation)) {
        return Status(StatusCode::kFencedIncarnation,
                      "lease was issued by incarnation " +
                          std::to_string(grant.incarnation.value) + ", token claims " +
                          std::to_string(token.incarnation.value));
    }
    switch (grant.state) {
        case GrantState::kReleased:
            return Status(StatusCode::kRefused, "grant has been released");
        case GrantState::kExpired:
            return Status(StatusCode::kExpired, "grant has expired");
        case GrantState::kFenced:
            return Status(StatusCode::kFencedIncarnation, "grant was fenced: " + grant.reason);
        case GrantState::kRecovering:
        case GrantState::kPending:
            // The incarnation check comes after this branch on purpose. A
            // recovered grant is expected to carry the previous incarnation,
            // and the honest answer is "the durable commit is ambiguous until
            // it is revalidated", not "this token is forged".
            return Status(StatusCode::kIndeterminate,
                          "grant was recovered from durable state and has not been revalidated");
        case GrantState::kSuspended:
            return Status(StatusCode::kLifecycleRefused, "grant is suspended: " + grant.reason);
        case GrantState::kActive:
            break;
    }
    if (!(grant.incarnation == incarnation_)) {
        return Status(StatusCode::kFencedIncarnation,
                      "lease belongs to a superseded daemon incarnation");
    }
    if (grant.expires_at_ms <= now_ms) {
        return Status(StatusCode::kExpired, "grant deadline has passed");
    }
    if (!(grant.epoch == snapshot.epoch())) {
        return Status(StatusCode::kStaleEpoch, "rack membership epoch has changed");
    }
    if (grant.generation.value > snapshot.generation().value) {
        return Status(StatusCode::kStaleGeneration, "grant names a later topology generation");
    }
    if (!lifecycle_honours_existing_authority(snapshot.lifecycle())) {
        return Status(StatusCode::kLifecycleRefused, "the rack lifecycle does not honour leases");
    }

    const Result<std::vector<ResourceRef>> expanded = expand_scope(snapshot, grant.scope);
    if (!expanded.has_value()) {
        return Status(StatusCode::kFencedIncarnation,
                      "grant scope no longer resolves in this rack: " + expanded.error().detail());
    }
    if (!(authority_basis_digest(snapshot, *expanded) == grant.authority_basis)) {
        return Status(StatusCode::kFencedIncarnation,
                      "the resources this grant depends on changed since it was issued");
    }
    for (ResourceRef ref : *expanded) {
        const ResourceState* state = snapshot.find(ref);
        if (state == nullptr) {
            return Status(StatusCode::kFencedIncarnation,
                          "a resource in this grant's scope is no longer part of the rack");
        }
        if (state->eligibility != Eligibility::kEligible) {
            return Status(StatusCode::kScopeNotEligible,
                          "scope contains a resource that is not eligible: " + ref.to_text());
        }
        if (state->maintenance) {
            return Status(StatusCode::kScopeNotEligible,
                          "scope contains a resource inside a maintenance window: " +
                              ref.to_text());
        }
    }
    return grant;
}

Result<Grant> GrantRegistry::validate(const Snapshot& snapshot, const LeaseToken& token,
                                      TimestampMs now_ms) const {
    return check_lease(snapshot, token, now_ms);
}

Result<Grant> GrantRegistry::renew(const Snapshot& snapshot, const LeaseToken& token,
                                   std::uint64_t ttl_ms, TimestampMs now_ms) {
    if (ttl_ms < limits_.min_ttl_ms || ttl_ms > limits_.max_ttl_ms) {
        return Status(StatusCode::kOutOfRange, "requested lease lifetime is out of range");
    }
    const Result<Grant> checked = check_lease(snapshot, token, now_ms);
    if (!checked.has_value()) {
        return checked.error();
    }
    const auto grant_it = grants_.find(token.grant);
    if (grant_it == grants_.end()) {
        return Status(StatusCode::kNotFound, "no such grant");
    }
    std::uint64_t deadline = 0;
    if (!checked_add_u64(now_ms, ttl_ms, deadline)) {
        return Status(StatusCode::kOutOfRange, "lease deadline overflowed");
    }
    grant_it->second.expires_at_ms = deadline;
    return grant_it->second;
}

Status GrantRegistry::release(const LeaseToken& token, TimestampMs now_ms) {
    (void)now_ms;
    const auto grant_it = grants_.find(token.grant);
    if (grant_it == grants_.end()) {
        return Status(StatusCode::kNotFound, "no such grant");
    }
    Grant& grant = grant_it->second;
    if (grant.fence != token.fence || !(grant.authority_basis == token.scope_basis) ||
        !(grant.incarnation == token.incarnation)) {
        return Status(StatusCode::kFencedIncarnation,
                      "release token does not match the grant record");
    }
    if (!is_live(grant.state)) {
        return Status{};
    }
    if (grant.state == GrantState::kRecovering && recovering_ > 0) {
        --recovering_;
    }
    detach(grant);
    grant.state = GrantState::kReleased;
    grant.reason = "released by holder";
    if (live_ > 0) {
        --live_;
    }
    return Status{};
}

namespace {
/// Shared fencing loop used by fence_all, fence_device and expire_due.
void retire_grant(Grant& grant, GrantState state, const std::string& reason) {
    grant.state = state;
    grant.reason = reason;
}
}  // namespace

void GrantRegistry::fence_all(std::string reason) {
    for (auto& entry : grants_) {
        Grant& grant = entry.second;
        if (!is_live(grant.state)) {
            continue;
        }
        if (grant.state == GrantState::kRecovering && recovering_ > 0) {
            --recovering_;
        }
        detach(grant);
        retire_grant(grant, GrantState::kFenced, reason);
        if (live_ > 0) {
            --live_;
        }
    }
}

void GrantRegistry::fence_device(const Snapshot& snapshot, DeviceId device, std::string reason) {
    const Result<std::vector<ResourceRef>> device_scope =
        expand_scope(snapshot, ResourceRef{ResourceKind::kDevice, device.value()});
    for (auto& entry : grants_) {
        Grant& grant = entry.second;
        if (!is_live(grant.state)) {
            continue;
        }
        bool touches = false;
        if (device_scope.has_value()) {
            const Result<std::vector<ResourceRef>> expanded = expand_scope(snapshot, grant.scope);
            if (expanded.has_value() && scopes_intersect(*expanded, *device_scope)) {
                touches = true;
            }
        }
        if (!touches) {
            for (ResourceRef ref : grant.capacity_pool) {
                if (ref.kind == ResourceKind::kDevice && ref.id == device.value()) {
                    touches = true;
                    break;
                }
            }
        }
        if (!touches) {
            continue;
        }
        if (grant.state == GrantState::kRecovering && recovering_ > 0) {
            --recovering_;
        }
        detach(grant);
        retire_grant(grant, GrantState::kFenced, reason);
        if (live_ > 0) {
            --live_;
        }
    }
}

std::vector<GrantId> GrantRegistry::expire_due(TimestampMs now_ms) {
    std::vector<GrantId> expired;
    for (auto& entry : grants_) {
        Grant& grant = entry.second;
        if (!is_live(grant.state)) {
            continue;
        }
        if (grant.expires_at_ms > now_ms) {
            continue;
        }
        if (grant.state == GrantState::kRecovering && recovering_ > 0) {
            --recovering_;
        }
        detach(grant);
        retire_grant(grant, GrantState::kExpired, "lease deadline passed");
        if (live_ > 0) {
            --live_;
        }
        expired.push_back(grant.id);
    }
    return expired;
}

void GrantRegistry::suspend_all(std::string reason) {
    for (auto& entry : grants_) {
        Grant& grant = entry.second;
        if (grant.state != GrantState::kActive) {
            continue;
        }
        grant.state = GrantState::kSuspended;
        grant.reason = reason;
    }
}

std::size_t GrantRegistry::resume_all(const Snapshot& snapshot, TimestampMs now_ms) {
    std::size_t resumed = 0;
    for (auto& entry : grants_) {
        Grant& grant = entry.second;
        if (grant.state != GrantState::kSuspended) {
            continue;
        }
        const Result<std::vector<ResourceRef>> expanded = expand_scope(snapshot, grant.scope);
        bool ok = expanded.has_value() && grant.expires_at_ms > now_ms &&
                  grant.epoch == snapshot.epoch() &&
                  authority_basis_digest(snapshot, *expanded) == grant.authority_basis;
        if (ok) {
            for (ResourceRef ref : *expanded) {
                const ResourceState* state = snapshot.find(ref);
                if (state == nullptr || state->eligibility != Eligibility::kEligible) {
                    ok = false;
                    break;
                }
            }
        }
        if (ok) {
            grant.state = GrantState::kActive;
            grant.reason.clear();
            ++resumed;
        } else {
            detach(grant);
            retire_grant(grant, GrantState::kFenced, "suspended lease could not be revalidated");
            if (live_ > 0) {
                --live_;
            }
        }
    }
    return resumed;
}

Status GrantRegistry::install_recovered(const Snapshot& snapshot, Grant grant) {
    if (!snapshot.valid()) {
        return Status(StatusCode::kInvalidArgument, "snapshot handle is empty");
    }
    if (!grant.id.is_set()) {
        return Status(StatusCode::kInvalidIdentity, "recovered grant has no identity");
    }
    if (grants_.find(grant.id) != grants_.end()) {
        return Status(StatusCode::kDuplicateIdentity, "recovered grant is already installed");
    }
    if (live_ >= limits_.max_live_grants) {
        return Status(StatusCode::kResourceExhausted, "live grant bound reached during recovery");
    }
    next_fence_ = std::max(next_fence_, grant.fence);
    if (grant.capacity_pool.empty()) {
        const Result<std::vector<ResourceRef>> pool = capacity_pool_refs(snapshot, grant.scope);
        if (!pool.has_value()) {
            grant.state = GrantState::kFenced;
            if (grant.reason.empty()) {
                grant.reason = "scope no longer resolves after restart";
            }
            grants_[grant.id] = grant;
            return Status{};
        }
        grant.capacity_pool = *pool;
    }
    grant.state = GrantState::kRecovering;
    if (grant.reason.empty()) {
        grant.reason = "recovered from durable state; awaiting revalidation";
    }
    attach(snapshot, grant);
    grants_[grant.id] = grant;
    ++live_;
    ++recovering_;
    return Status{};
}

Result<std::size_t> GrantRegistry::revalidate(const Snapshot& snapshot,
                                              ControllerIncarnation incarnation,
                                              TimestampMs now_ms) {
    incarnation_ = incarnation;
    std::size_t rearmed = 0;
    for (auto& entry : grants_) {
        Grant& grant = entry.second;
        if (grant.state != GrantState::kRecovering) {
            continue;
        }
        const Result<std::vector<ResourceRef>> expanded = expand_scope(snapshot, grant.scope);
        bool ok = expanded.has_value();
        std::string failure;
        if (ok) {
            if (grant.expires_at_ms <= now_ms) {
                ok = false;
                failure = "lease expired while the daemon was down";
            } else if (!(grant.epoch == snapshot.epoch())) {
                ok = false;
                failure = "rack membership epoch changed while the daemon was down";
            } else if (grant.generation.value > snapshot.generation().value) {
                ok = false;
                failure = "durable grant names a generation the rack has not reached";
            } else if (!(authority_basis_digest(snapshot, *expanded) == grant.authority_basis)) {
                ok = false;
                failure = "the resources this grant depends on changed while the daemon was down";
            } else {
                for (ResourceRef ref : *expanded) {
                    const ResourceState* state = snapshot.find(ref);
                    if (state == nullptr || state->eligibility != Eligibility::kEligible) {
                        ok = false;
                        failure = "scope contains a resource that is not eligible: " +
                                  ref.to_text();
                        break;
                    }
                }
            }
        } else {
            failure = "scope no longer resolves: " + expanded.error().detail();
        }

        if (ok) {
            grant.state = GrantState::kActive;
            grant.incarnation = incarnation;
            grant.fence = ++next_fence_;
            grant.issued_at_ms = now_ms;
            grant.reason.clear();
            ++rearmed;
        } else {
            detach(grant);
            retire_grant(grant, GrantState::kFenced, failure);
            if (live_ > 0) {
                --live_;
            }
        }
        if (recovering_ > 0) {
            --recovering_;
        }
    }
    return rearmed;
}

void GrantRegistry::install_outcome(const RequestId& request, RequestOutcome outcome) {
    if (!request.is_set()) {
        return;
    }
    if (outcomes_.size() >= limits_.max_remembered_requests &&
        outcomes_.find(request) == outcomes_.end()) {
        return;
    }
    outcomes_[request] = std::move(outcome);
}

Status GrantRegistry::apply_state(GrantId id, GrantState state, const std::string& reason) {
    const auto it = grants_.find(id);
    if (it == grants_.end()) {
        return Status(StatusCode::kNotFound, "no such grant");
    }
    Grant& grant = it->second;
    if (state == GrantState::kReleased || state == GrantState::kFenced ||
        state == GrantState::kExpired) {
        if (is_live(grant.state)) {
            if (grant.state == GrantState::kRecovering && recovering_ > 0) {
                --recovering_;
            }
            detach(grant);
            if (live_ > 0) {
                --live_;
            }
        }
        grant.state = state;
        grant.reason = reason;
        return Status{};
    }
    return Status(StatusCode::kInvalidArgument,
                  "only terminal grant states may be applied from a log record");
}

Result<RequestOutcome> GrantRegistry::lookup_request(const RequestId& request) const {
    const auto it = outcomes_.find(request);
    if (it == outcomes_.end()) {
        return Status(StatusCode::kUnknown,
                      "this daemon has no record of the request; it was either refused before "
                      "being durably remembered or it belongs to a store that was discarded");
    }
    return it->second;
}

const Grant* GrantRegistry::find(const GrantId& id) const {
    const auto it = grants_.find(id);
    return it == grants_.end() ? nullptr : &it->second;
}

std::vector<Grant> GrantRegistry::list() const {
    std::vector<Grant> out;
    out.reserve(grants_.size());
    for (const auto& entry : grants_) {
        out.push_back(entry.second);
    }
    return out;
}

std::size_t GrantRegistry::live_count() const noexcept {
    return live_;
}

std::size_t GrantRegistry::recovering_count() const noexcept {
    return recovering_;
}

Result<std::vector<GrantId>> GrantRegistry::conflicts_bruteforce(
    const Snapshot& snapshot, const GrantRequest& request) const {
    // Independent reference model: recompute every live grant's footprint from
    // the snapshot and compare pairwise. Deliberately index free, so the
    // differential test compares two genuinely different algorithms.
    const Result<std::vector<ResourceRef>> expanded = expand_scope(snapshot, request.scope);
    if (!expanded.has_value()) {
        return expanded.error();
    }
    std::vector<GrantId> conflicts;
    for (const auto& entry : grants_) {
        const Grant& grant = entry.second;
        if (!is_live(grant.state)) {
            continue;
        }
        if (!occupant_conflicts(request.mode, grant.mode)) {
            continue;
        }
        const Result<std::vector<ResourceRef>> other = expand_scope(snapshot, grant.scope);
        if (!other.has_value()) {
            continue;
        }
        if (scopes_intersect(*expanded, *other)) {
            conflicts.push_back(grant.id);
        }
    }
    std::sort(conflicts.begin(), conflicts.end());
    return conflicts;
}





}  // namespace rnf
