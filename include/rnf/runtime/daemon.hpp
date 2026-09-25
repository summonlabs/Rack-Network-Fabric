// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The daemon core owns the rack's authoritative state. It is deliberately free
// of any networking: every method is a synchronous, thread safe operation on
// one in-process state machine, which is what makes the multiprocess tests
// meaningful (the network layer is a thin veneer over these calls).

#ifndef RNF_RUNTIME_DAEMON_HPP
#define RNF_RUNTIME_DAEMON_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rnf/authority/registry.hpp"
#include "rnf/net/protocol.hpp"
#include "rnf/persist/store.hpp"

namespace rnf {

struct DaemonConfig {
    RackConfig rack;
    /// Empty means the daemon keeps no durable state. The durability proofs all
    /// run with a real store directory.
    std::filesystem::path store_directory;
    std::string bind_host = "127.0.0.1";
    std::uint16_t port = 0;
    std::size_t max_connections = 32;
    std::uint32_t max_frame_payload = kDefaultMaxFramePayload;
    std::uint64_t default_ttl_ms = 60000;
    /// Compose automatically on start and after accepted evidence when true.
    bool auto_compose = false;
    /// Checkpoint after this many accepted log records; 0 disables the
    /// automatic checkpoint.
    std::uint64_t checkpoint_every_records = 4096;
    EvidenceLimits evidence_limits{};
    ComposeLimits compose_limits{};
    AuthorityLimits authority_limits{};
    std::size_t max_records_per_batch = 4096;
    /// Write through to stable storage on every accepted mutation.
    bool fsync_on_append = true;
};

/// Daemon counters, reported by the stats command.
struct DaemonStats {
    std::uint64_t uptime_ms = 0;
    std::uint64_t evidence_records = 0;
    std::uint64_t evidence_sources = 0;
    std::uint64_t evidence_conflicts = 0;
    std::uint64_t evidence_superseded = 0;
    std::uint64_t log_records = 0;
    std::uint64_t log_bytes = 0;
    std::uint64_t checkpoints = 0;
    std::uint64_t requests_served = 0;
    std::uint64_t connections_accepted = 0;
    std::uint64_t connections_rejected = 0;
    std::uint64_t frames_rejected = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t composes = 0;
    std::uint64_t grants_issued = 0;
    std::uint64_t grants_refused = 0;
    std::uint64_t grants_fenced = 0;
};

/// The authoritative state machine.
///
/// Threading: every public method takes the internal state mutex. No method
/// invokes a caller supplied callback, so no lock is ever held across foreign
/// code. The mutex is never held while a socket operation runs.
class DaemonCore {
public:
    explicit DaemonCore(DaemonConfig config);
    ~DaemonCore();
    DaemonCore(const DaemonCore&) = delete;
    DaemonCore& operator=(const DaemonCore&) = delete;

    /// Open durable state, replay it, bump the incarnation and compose.
    [[nodiscard]] Status start();

    /// Fence everything and mark the store cleanly closed.
    [[nodiscard]] Status stop(TimestampMs now_ms);

    [[nodiscard]] const DaemonConfig& config() const noexcept { return config_; }
    [[nodiscard]] ControllerIncarnation incarnation() const;
    [[nodiscard]] LifecycleState lifecycle() const;
    [[nodiscard]] const RecoveryReport* recovery_report() const;

    // -- commands -----------------------------------------------------------
    [[nodiscard]] Result<HelloResponse> hello(const HelloRequest& request);
    [[nodiscard]] Result<EvidenceAckResponse> submit_evidence(
        const SubmitEvidenceRequest& request);
    [[nodiscard]] Result<ComposeResponse> compose(const ComposeCommand& request);
    [[nodiscard]] Result<GrantResponse> acquire(const AcquireRequest& request);
    [[nodiscard]] Result<GrantResponse> renew(const RenewRequest& request);
    [[nodiscard]] Result<AckResponse> release(const TokenMessage& request);
    [[nodiscard]] Result<GrantResponse> validate(const TokenMessage& request);
    [[nodiscard]] Result<LookupResponse> lookup(const LookupRequest& request);
    [[nodiscard]] Result<StateResponse> query_state();
    [[nodiscard]] Result<ResourcesResponse> query_resources(const ResourceQuery& request);
    [[nodiscard]] Result<PathsResponse> query_paths();
    [[nodiscard]] Result<GrantsResponse> query_grants();
    [[nodiscard]] Result<LifecycleResponse> set_lifecycle(const LifecycleRequest& request);
    [[nodiscard]] Result<AckResponse> checkpoint();
    [[nodiscard]] Result<StatsResponse> stats();

    /// Expire leases whose deadline has passed. Called by the ticker thread and
    /// at the start of every mutating command.
    std::size_t expire_due(TimestampMs now_ms);

    void note_connection_accepted();
    void note_connection_rejected();
    void note_frame_rejected();
    void note_request_served();

private:
    [[nodiscard]] Status apply_recovered_record(const StoreRecord& record, bool during_recovery);
    std::size_t expire_due_locked(TimestampMs now_ms);
    [[nodiscard]] Status ensure_snapshot_locked();
    [[nodiscard]] Status append_locked(const StoreRecord& record);
    [[nodiscard]] Status maybe_checkpoint_locked();
    [[nodiscard]] Status compose_locked(const TopologyGeneration& target);
    [[nodiscard]] Result<GrantResponse> grant_to_response(const Grant& grant,
                                                         StatusCode code) const;
    [[nodiscard]] Status transition_lifecycle_locked(LifecycleState target, std::string& detail);
    [[nodiscard]] TimestampMs now_ms() const;

    DaemonConfig config_;
    mutable std::mutex mutex_;
    EvidenceLedger ledger_;
    GrantRegistry grants_;
    std::unique_ptr<Store> store_;
    Snapshot snapshot_;
    LifecycleState lifecycle_ = LifecycleState::kAssembling;
    TopologyGeneration generation_{};
    RackEpoch epoch_{};
    Digest member_set_digest_{};
    ControllerIncarnation incarnation_{};
    DaemonStats stats_;
    std::chrono::steady_clock::time_point start_time_;
    bool dirty_recovery_ = false;
    bool membership_initialized_ = false;
    bool started_ = false;
    bool stopped_ = false;
};

}  // namespace rnf

#endif  // RNF_RUNTIME_DAEMON_HPP
