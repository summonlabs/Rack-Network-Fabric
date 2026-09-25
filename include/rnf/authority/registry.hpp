// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The grant registry. It is the only component that decides whether authority
// may exist, and it answers every question from an immutable snapshot plus its
// own bookkeeping. It never reads live device state.

#ifndef RNF_AUTHORITY_REGISTRY_HPP
#define RNF_AUTHORITY_REGISTRY_HPP

#include <map>
#include <string>
#include <vector>

#include "rnf/authority/grant.hpp"
#include "rnf/compose/composer.hpp"

namespace rnf {

struct AuthorityLimits {
    std::size_t max_live_grants = 4096;
    std::size_t max_remembered_requests = 65536;
    std::uint64_t max_ttl_ms = 24ULL * 60ULL * 60ULL * 1000ULL;
    std::uint64_t min_ttl_ms = 1;
    /// Total number of (grant, resource) pairs the exclusivity index may hold.
    /// A grant whose scope would exceed the remaining budget is refused rather
    /// than admitted with a partially indexed scope.
    std::size_t max_occupancy_entries = 1U << 20;
};

[[nodiscard]] const AuthorityLimits& default_authority_limits() noexcept;

/// The remembered outcome of a mutating request, so a replayed request returns
/// the original answer rather than creating a second grant.
struct RequestOutcome {
    StatusCode code = StatusCode::kOk;
    GrantId grant{};
    FencingToken fence = 0;
    ControllerIncarnation incarnation{};
    Digest request_digest{};
    std::string detail;
};

/// Threading: not internally synchronised. rnfd holds its state mutex across
/// every call. No method invokes a callback, so no lock is ever held across
/// foreign code.
class GrantRegistry {
public:
    explicit GrantRegistry(AuthorityLimits limits = default_authority_limits())
        : limits_(limits) {}

    /// Acquire authority. Idempotent on GrantRequest::request.
    [[nodiscard]] Result<Grant> acquire(const Snapshot& snapshot, const GrantRequest& request,
                                        ControllerIncarnation incarnation, TimestampMs now_ms,
                                        bool allow_new_authority);

    /// Extend an existing lease. Requires the grant to validate as-is.
    [[nodiscard]] Result<Grant> renew(const Snapshot& snapshot, const LeaseToken& token,
                                      std::uint64_t ttl_ms, TimestampMs now_ms);

    /// Return a lease early. Idempotent for an already released grant.
    [[nodiscard]] Status release(const LeaseToken& token, TimestampMs now_ms);

    /// Check whether a lease may be used right now.
    [[nodiscard]] Result<Grant> validate(const Snapshot& snapshot, const LeaseToken& token,
                                         TimestampMs now_ms) const;

    /// Reverse lookup used to resolve durable-commit ambiguity after a restart.
    [[nodiscard]] Result<RequestOutcome> lookup_request(const RequestId& request) const;
    [[nodiscard]] const Grant* find(const GrantId& id) const;

    /// Move every live grant into kFenced and release its capacity.
    void fence_all(std::string reason);
    /// Fence every grant whose scope touched a device.
    void fence_device(const Snapshot& snapshot, DeviceId device, std::string reason);
    /// Expire leases whose deadline has passed, returning exactly the grants
    /// that transitioned so the caller can durably record each transition once.
    std::vector<GrantId> expire_due(TimestampMs now_ms);
    /// Mark live grants suspended (used when the rack enters maintenance).
    void suspend_all(std::string reason);
    /// Re-arm suspended grants that still validate.
    [[nodiscard]] std::size_t resume_all(const Snapshot& snapshot, TimestampMs now_ms);

    /// Install a grant loaded from durable state. It lands in kRecovering and
    /// reserves its capacity but cannot authorize anything.
    [[nodiscard]] Status install_recovered(const Snapshot& snapshot, Grant grant);

    /// Install a remembered request outcome loaded from durable state, so that
    /// a client replaying a request after a restart still gets the original
    /// answer instead of a second grant.
    void install_outcome(const RequestId& request, RequestOutcome outcome);

    /// Apply a durable grant state transition (release, fence, expiry).
    [[nodiscard]] Status apply_state(GrantId id, GrantState state, const std::string& reason);

    /// Try to re-issue every recovering grant under the current incarnation.
    /// Returns the number of grants that were re-armed.
    [[nodiscard]] Result<std::size_t> revalidate(const Snapshot& snapshot,
                                                 ControllerIncarnation incarnation,
                                                 TimestampMs now_ms);

    /// Capacity accounting.
    /// The incarnation this registry currently issues under. A lease carrying
    /// any other incarnation is refused without further inspection.
    void set_incarnation(ControllerIncarnation incarnation) noexcept { incarnation_ = incarnation; }
    [[nodiscard]] ControllerIncarnation incarnation() const noexcept { return incarnation_; }

    /// Drop occupancy entries that belong to grants which are no longer live.
    /// Every fencing path already detaches exactly, so a non-zero return value
    /// indicates an internal bookkeeping defect and is reported by the tests.
    std::size_t prune();

    /// Number of occupancy index entries currently held. Bounded by
    /// AuthorityLimits::max_occupancy_entries.
    [[nodiscard]] std::size_t occupancy_entries() const noexcept { return occupancy_entries_; }

    [[nodiscard]] Result<Capacity> available(const Snapshot& snapshot, ResourceRef ref) const;
    [[nodiscard]] Result<Capacity> committed(ResourceRef ref) const;
    [[nodiscard]] Capacity committed_total() const noexcept { return committed_total_; }

    [[nodiscard]] std::vector<Grant> list() const;
    [[nodiscard]] std::size_t live_count() const noexcept;
    [[nodiscard]] std::size_t recovering_count() const noexcept;
    [[nodiscard]] const std::map<RequestId, RequestOutcome>& outcomes() const noexcept {
        return outcomes_;
    }

    /// Brute-force conflict scan used by the differential test model.
    [[nodiscard]] Result<std::vector<GrantId>> conflicts_bruteforce(
        const Snapshot& snapshot, const GrantRequest& request) const;

private:
    struct Occupant {
        GrantId id{};
        GrantMode mode = GrantMode::kShared;
    };

    [[nodiscard]] bool is_live(GrantState state) const noexcept;
    void attach(const Snapshot& snapshot, const Grant& grant);
    [[nodiscard]] Result<std::vector<ResourceRef>> footprint(const Snapshot& snapshot,
                                                             ResourceRef root) const;
    [[nodiscard]] Result<Grant> check_lease(const Snapshot& snapshot, const LeaseToken& token,
                                            TimestampMs now_ms) const;
    void remember(const GrantRequest& request, const RequestOutcome& outcome);

    void detach(const Grant& grant);
    [[nodiscard]] Status check_capacity(const Snapshot& snapshot,
                                        const std::vector<ResourceRef>& pool,
                                        Capacity requested) const;

    AuthorityLimits limits_;
    std::map<GrantId, Grant> grants_;
    std::map<RequestId, RequestOutcome> outcomes_;
    std::map<ResourceRef, std::vector<Occupant>> occupancy_;
    std::map<GrantId, std::vector<ResourceRef>> footprints_;
    std::size_t occupancy_entries_ = 0;
    std::map<ResourceRef, std::uint64_t> committed_;
    Capacity committed_total_{};
    FencingToken next_fence_ = 0;
    ControllerIncarnation incarnation_{};
    std::size_t live_ = 0;
    std::size_t recovering_ = 0;
};

}  // namespace rnf

#endif  // RNF_AUTHORITY_REGISTRY_HPP
