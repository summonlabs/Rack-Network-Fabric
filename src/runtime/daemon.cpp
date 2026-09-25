// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/runtime/daemon.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <utility>

#include "rnf/core/log.hpp"
#include "rnf/net/socket.hpp"

namespace rnf {
namespace {

constexpr std::size_t kMaxStateDiagnostics = 256;

[[nodiscard]] std::string join_detail(const Status& status) {
    return status.describe();
}

[[nodiscard]] bool accepts_compose(LifecycleState state) noexcept {
    return state != LifecycleState::kRetired;
}

}  // namespace

DaemonCore::DaemonCore(DaemonConfig config) : config_(std::move(config)) {
    ledger_ = EvidenceLedger(config_.evidence_limits);
    grants_ = GrantRegistry(config_.authority_limits);
    start_time_ = std::chrono::steady_clock::now();
}

DaemonCore::~DaemonCore() = default;

TimestampMs DaemonCore::now_ms() const {
    return static_cast<TimestampMs>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

ControllerIncarnation DaemonCore::incarnation() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return incarnation_;
}

LifecycleState DaemonCore::lifecycle() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return lifecycle_;
}

const RecoveryReport* DaemonCore::recovery_report() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return store_ ? &store_->recovery() : nullptr;
}

// ---------------------------------------------------------------------------
// Durability helpers
// ---------------------------------------------------------------------------

Status DaemonCore::append_locked(const StoreRecord& record) {
    if (!store_) {
        return Status{};
    }
    RNF_TRYV(store_->append(record));
    ++stats_.log_records;
    stats_.log_bytes = store_->log_bytes();
    return Status{};
}

Status DaemonCore::maybe_checkpoint_locked() {
    if (!store_ || config_.checkpoint_every_records == 0) {
        return Status{};
    }
    if (store_->appends() < config_.checkpoint_every_records) {
        return Status{};
    }
    if (!snapshot_.valid()) {
        return Status{};
    }
    const Result<std::vector<std::uint8_t>> encoded = snapshot_.encode();
    if (!encoded.has_value()) {
        return encoded.error();
    }
    Checkpoint checkpoint;
    checkpoint.wal_offset = store_->log_bytes();
    checkpoint.generation = snapshot_.generation();
    checkpoint.epoch = snapshot_.epoch();
    checkpoint.lifecycle = lifecycle_;
    checkpoint.member_set_digest = member_set_digest_;
    checkpoint.incarnation = incarnation_;
    checkpoint.snapshot_digest = snapshot_.digest();
    checkpoint.ledger_digest = ledger_.digest();
    checkpoint.snapshot_bytes = *encoded;
    checkpoint.ledger = ledger_.records();
    RNF_TRYV(store_->write_checkpoint(checkpoint));
    ++stats_.checkpoints;
    return Status{};
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

Status DaemonCore::apply_recovered_record(const StoreRecord& record, bool during_recovery) {
    (void)during_recovery;
    switch (record.type) {
        case RecordType::kRackBind: {
            const auto& body = std::get<RackBindRecord>(record.payload);
            if (body.rack.is_set() && !(body.rack == config_.rack.rack)) {
                return Status(StatusCode::kOutOfRack,
                              "the store belongs to rack " + body.rack.to_hex() +
                                  " but this daemon is configured for " +
                                  config_.rack.rack.to_hex());
            }
            return Status{};
        }
        case RecordType::kConfig: {
            const auto& body = std::get<ConfigRecord>(record.payload);
            config_.rack.headroom_floor = body.headroom_floor;
            config_.rack.max_paths_per_pair = static_cast<std::size_t>(body.max_paths_per_pair);
            config_.rack.max_path_hops = static_cast<std::size_t>(body.max_path_hops);
            return Status{};
        }
        case RecordType::kEvidence: {
            const EvidenceRecord& evidence = std::get<EvidenceRecord>(record.payload);
            (void)ledger_.insert(evidence);
            return Status{};
        }
        case RecordType::kLifecycle: {
            const auto& body = std::get<LifecycleRecord>(record.payload);
            lifecycle_ = body.state;
            if (body.generation.value > generation_.value) {
                generation_ = body.generation;
            }
            if (body.epoch.value > epoch_.value) {
                epoch_ = body.epoch;
            }
            return Status{};
        }
        case RecordType::kGeneration: {
            generation_ = std::get<GenerationRecord>(record.payload).generation;
            return Status{};
        }
        case RecordType::kEpoch: {
            const auto& body = std::get<EpochRecord>(record.payload);
            epoch_ = body.epoch;
            member_set_digest_ = body.member_set_digest;
            membership_initialized_ = true;
            return Status{};
        }
        case RecordType::kCleanShutdown:
            return Status{};
        case RecordType::kGrantCommit:
        case RecordType::kGrantState:
        case RecordType::kRequestOutcome:
            // Handled by start(), which needs a composed snapshot first.
            return Status{};
    }
    return Status{};
}

Status DaemonCore::compose_locked(const TopologyGeneration& target) {
    if (!accepts_compose(lifecycle_)) {
        return Status(StatusCode::kLifecycleRefused,
                      "a retired rack does not compose new state");
    }
    ComposeInput request;
    request.generation = target;
    request.epoch = epoch_;
    request.lifecycle = lifecycle_;
    request.limits = config_.compose_limits;

    Result<ComposeOutcome> outcome = ::rnf::compose(ledger_, config_.rack, request);
    if (!outcome.has_value()) {
        return outcome.error();
    }
    if (!membership_initialized_) {
        // The first composition adopts the member set it finds. It is not a
        // membership change, so it does not advance the epoch.
        membership_initialized_ = true;
        member_set_digest_ = outcome->snapshot->member_set_digest;
    } else if (!(outcome->snapshot->member_set_digest == member_set_digest_)) {
        // The member set changed, so the rack epoch advances. Everything bound
        // to the previous epoch is fenced the moment a lease is presented.
        if (epoch_.value == std::numeric_limits<std::uint64_t>::max()) {
            return Status(StatusCode::kResourceExhausted, "the rack epoch counter is exhausted");
        }
        epoch_.value += 1;
        member_set_digest_ = outcome->snapshot->member_set_digest;
        StoreRecord epoch_record;
        epoch_record.type = RecordType::kEpoch;
        EpochRecord body;
        body.epoch = epoch_;
        body.member_set_digest = member_set_digest_;
        epoch_record.payload = body;
        RNF_TRYV(append_locked(epoch_record));

        request.epoch = epoch_;
        outcome = ::rnf::compose(ledger_, config_.rack, request);
        if (!outcome.has_value()) {
            return outcome.error();
        }
    }
    snapshot_ = outcome->snapshot;
    generation_ = snapshot_.generation();
    ++stats_.composes;
    return Status{};
}

Status DaemonCore::ensure_snapshot_locked() {
    if (snapshot_.valid()) {
        return Status{};
    }
    return compose_locked(generation_);
}

Status DaemonCore::start() {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (started_) {
        return Status{};
    }
    started_ = true;
    start_time_ = std::chrono::steady_clock::now();

    std::vector<Grant> recovered_grants;
    std::vector<std::pair<GrantId, GrantStateRecord>> recovered_states;

    if (!config_.store_directory.empty()) {
        StoreOptions options;
        options.fsync_on_append = config_.fsync_on_append;
        Result<std::unique_ptr<Store>> opened = Store::open(config_.store_directory, options);
        if (!opened.has_value()) {
            return opened.error();
        }
        store_ = std::move(*opened);

        const RackId bound = store_->bound_rack();
        if (bound.is_set() && !(bound == config_.rack.rack)) {
            return Status(StatusCode::kOutOfRack,
                          "the store at " + config_.store_directory.string() +
                              " belongs to rack " + bound.to_hex());
        }
        // A store that has just been created is not a dirty recovery: there is
        // no previous incarnation whose state could be ambiguous.
        dirty_recovery_ =
            !store_->recovery().fresh_store && !store_->recovery().clean_shutdown;

        if (store_->recovered().checkpoint.present) {
            const Checkpoint& checkpoint = store_->recovered().checkpoint;
            for (const EvidenceRecord& record : checkpoint.ledger) {
                (void)ledger_.insert(record);
            }
            // The checkpoint sits at a log offset, so the epoch, lifecycle and
            // generation records that precede it are not replayed. They are
            // carried in the checkpoint itself; without this the first
            // composition after a restart would look like a membership change
            // and advance the epoch, fencing every existing lease.
            generation_ = checkpoint.generation;
            epoch_ = checkpoint.epoch;
            lifecycle_ = checkpoint.lifecycle;
            member_set_digest_ = checkpoint.member_set_digest;
            membership_initialized_ = true;
        }
        for (const StoreRecord& record : store_->recovered().records) {
            switch (record.type) {
                case RecordType::kGrantCommit:
                    recovered_grants.push_back(std::get<Grant>(record.payload));
                    break;
                case RecordType::kGrantState:
                    recovered_states.emplace_back(
                        std::get<GrantStateRecord>(record.payload).id,
                        std::get<GrantStateRecord>(record.payload));
                    break;
                case RecordType::kRequestOutcome: {
                    const auto& body = std::get<RequestOutcomeRecord>(record.payload);
                    RequestOutcome outcome;
                    outcome.code = body.code;
                    outcome.grant = body.grant;
                    outcome.fence = body.fence;
                    outcome.incarnation = body.incarnation;
                    outcome.request_digest = body.request_digest;
                    outcome.detail = body.detail;
                    grants_.install_outcome(body.request, std::move(outcome));
                    break;
                }
                default: {
                    const Status applied = apply_recovered_record(record, true);
                    if (!applied.ok()) {
                        return applied;
                    }
                    break;
                }
            }
        }

        Result<ControllerIncarnation> next = store_->next_incarnation();
        if (!next.has_value()) {
            return next.error();
        }
        incarnation_ = *next;
        RNF_TRYV(store_->bind_rack(config_.rack.rack));
        stats_.log_bytes = store_->log_bytes();
        stats_.log_records = store_->recovery().records_replayed;
    } else {
        incarnation_ = ControllerIncarnation{1};
    }

    grants_.set_incarnation(incarnation_);
    if (dirty_recovery_ && store_) {
        // The previous process died without marking the store closed. Nothing
        // recovered from it may authorize use until an operator has seen the
        // state and moved the rack forward, so the rack comes up recovering.
        lifecycle_ = LifecycleState::kRecovering;
    } else if (lifecycle_ == LifecycleState::kRecovering) {
        lifecycle_ = LifecycleState::kAssembling;
    }
    RNF_TRYV(compose_locked(generation_));

    for (const Grant& grant : recovered_grants) {
        const Status installed = grants_.install_recovered(snapshot_, grant);
        if (!installed.ok()) {
            RNF_LOG(LogLevel::kWarn, "could not install a recovered grant: " +
                                         installed.describe());
        }
    }
    for (const auto& entry : recovered_states) {
        (void)grants_.apply_state(entry.first, entry.second.state, entry.second.reason);
    }

    const Result<std::size_t> rearmed =
        grants_.revalidate(snapshot_, incarnation_, now_ms());
    if (!rearmed.has_value()) {
        return rearmed.error();
    }
    return Status{};
}

Status DaemonCore::stop(TimestampMs now_ms) {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (stopped_) {
        return Status{};
    }
    stopped_ = true;
    grants_.fence_all("daemon stopped");
    ++stats_.grants_fenced;
    if (store_) {
        RNF_TRYV(store_->mark_clean_shutdown(now_ms));
    }
    return Status{};
}

std::size_t DaemonCore::expire_due_locked(TimestampMs now_ms) {
    const std::vector<GrantId> expired = grants_.expire_due(now_ms);
    for (GrantId id : expired) {
        const Grant* grant = grants_.find(id);
        StoreRecord record;
        record.type = RecordType::kGrantState;
        GrantStateRecord body;
        body.id = id;
        body.state = GrantState::kExpired;
        body.reason = grant != nullptr ? grant->reason : std::string("expired");
        record.payload = std::move(body);
        (void)append_locked(record);
    }
    return expired.size();
}

std::size_t DaemonCore::expire_due(TimestampMs now_ms) {
    const std::lock_guard<std::mutex> guard(mutex_);
    return expire_due_locked(now_ms);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

Result<HelloResponse> DaemonCore::hello(const HelloRequest& request) {
    const std::lock_guard<std::mutex> guard(mutex_);
    HelloResponse response;
    response.protocol_version = kProtocolVersion;
    if (request.protocol_version != kProtocolVersion) {
        response.accepted = false;
        response.detail = "protocol version " + std::to_string(request.protocol_version) +
                          " is not supported; this daemon speaks " +
                          std::to_string(kProtocolVersion);
        return response;
    }
    response.accepted = true;
    response.rack = config_.rack.rack;
    response.epoch = epoch_;
    response.generation = generation_;
    response.incarnation = incarnation_;
    response.lifecycle = lifecycle_;
    response.snapshot_digest = snapshot_.valid() ? snapshot_.digest() : Digest{};
    response.detail = config_.rack.name;
    return response;
}

Result<EvidenceAckResponse> DaemonCore::submit_evidence(const SubmitEvidenceRequest& request) {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (lifecycle_ == LifecycleState::kRetired) {
        return Status(StatusCode::kLifecycleRefused, "a retired rack accepts no new evidence");
    }
    if (request.records.size() > config_.max_records_per_batch) {
        return Status(StatusCode::kOversizeField,
                      "evidence batch exceeds the configured daemon bound");
    }
    (void)expire_due_locked(now_ms());

    EvidenceAckResponse response;
    response.entries.reserve(request.records.size());
    for (const EvidenceRecord& record : request.records) {
        EvidenceAckEntry entry;
        entry.digest = evidence_digest(record);

        const Status valid = validate_evidence(record);
        if (!valid.ok()) {
            entry.outcome = InsertOutcome::kRefused;
            entry.code = valid.code();
            entry.detail = valid.detail();
            response.entries.push_back(std::move(entry));
            continue;
        }
        if (record.kind == EvidenceKind::kMember) {
            const auto& claim = std::get<MemberClaim>(record.payload);
            if (!(claim.rack == config_.rack.rack)) {
                entry.outcome = InsertOutcome::kRefused;
                entry.code = StatusCode::kOutOfRack;
                entry.detail = "membership claim names rack " + claim.rack.to_hex() +
                               ", this daemon is authoritative for " +
                               config_.rack.rack.to_hex();
                response.entries.push_back(std::move(entry));
                continue;
            }
        }

        StoreRecord store_record;
        store_record.type = RecordType::kEvidence;
        store_record.payload = record;
        const Status appended = append_locked(store_record);
        if (!appended.ok()) {
            entry.outcome = InsertOutcome::kRefused;
            entry.code = appended.code();
            entry.detail = "the record could not be committed durably: " + appended.detail();
            response.entries.push_back(std::move(entry));
            continue;
        }

        const InsertReport report = ledger_.insert(record);
        entry.outcome = report.outcome;
        entry.code = report.status.code();
        entry.detail = report.status.detail();
        response.entries.push_back(std::move(entry));
    }

    if (config_.auto_compose) {
        (void)compose_locked(generation_);
    }
    (void)maybe_checkpoint_locked();
    return response;
}

Result<ComposeResponse> DaemonCore::compose(const ComposeCommand& request) {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (lifecycle_ == LifecycleState::kRetired) {
        return Status(StatusCode::kLifecycleRefused, "a retired rack does not compose");
    }
    if (request.generation.value < generation_.value) {
        return Status(StatusCode::kStaleGeneration,
                      "requested generation " + std::to_string(request.generation.value) +
                          " is older than the current generation " +
                          std::to_string(generation_.value));
    }

    if (request.generation.value > generation_.value) {
        StoreRecord record;
        record.type = RecordType::kGeneration;
        GenerationRecord body;
        body.generation = request.generation;
        record.payload = body;
        RNF_TRYV(append_locked(record));
        generation_ = request.generation;
    }

    const Status composed = compose_locked(generation_);
    if (!composed.ok()) {
        ComposeResponse response;
        response.code = composed.code();
        response.generation = generation_;
        response.epoch = epoch_;
        response.incarnation = incarnation_;
        response.detail = composed.detail();
        return response;
    }
    (void)maybe_checkpoint_locked();

    ComposeResponse response;
    response.code = StatusCode::kOk;
    response.generation = snapshot_.generation();
    response.epoch = snapshot_.epoch();
    response.incarnation = incarnation_;
    response.digest = snapshot_.digest();
    response.diagnostics = static_cast<std::uint32_t>(snapshot_->diagnostics.size());
    return response;
}

Result<GrantResponse> DaemonCore::grant_to_response(const Grant& grant, StatusCode code) const {
    GrantResponse response;
    response.code = code;
    response.grant = GrantSummary::from(grant);
    response.detail = grant.reason;
    return response;
}

Result<GrantResponse> DaemonCore::acquire(const AcquireRequest& request) {
    const std::lock_guard<std::mutex> guard(mutex_);
    const TimestampMs now = now_ms();
    (void)expire_due_locked(now);
    const Status ready = ensure_snapshot_locked();
    if (!ready.ok()) {
        return ready;
    }

    GrantRequest grant_request;
    grant_request.request = request.request;
    grant_request.principal = request.principal;
    grant_request.scope = request.scope;
    grant_request.mode = request.mode;
    grant_request.capacity = request.capacity;
    grant_request.ttl_ms = request.ttl_ms == 0 ? config_.default_ttl_ms : request.ttl_ms;

    const bool allow_new = lifecycle_accepts_new_authority(lifecycle_);
    const Result<Grant> granted = grants_.acquire(snapshot_, grant_request, incarnation_, now,
                                                  allow_new);
    if (!granted.has_value()) {
        ++stats_.grants_refused;
        GrantResponse response;
        response.code = granted.error().code();
        response.detail = granted.error().detail();
        return response;
    }

    StoreRecord commit;
    commit.type = RecordType::kGrantCommit;
    commit.payload = *granted;
    const Status committed = append_locked(commit);
    if (!committed.ok()) {
        (void)grants_.release(granted->token(), now);
        ++stats_.grants_refused;
        return committed;
    }

    StoreRecord outcome_record;
    outcome_record.type = RecordType::kRequestOutcome;
    RequestOutcomeRecord outcome;
    outcome.request = grant_request.request;
    outcome.code = StatusCode::kOk;
    outcome.grant = granted->id;
    outcome.fence = granted->fence;
    outcome.incarnation = granted->incarnation;
    outcome.request_digest = grant_digest(*granted);
    outcome.detail = "grant committed";
    outcome_record.payload = std::move(outcome);
    const Status remembered = append_locked(outcome_record);
    if (!remembered.ok()) {
        (void)grants_.release(granted->token(), now);
        return remembered;
    }

    ++stats_.grants_issued;
    (void)maybe_checkpoint_locked();
    return grant_to_response(*granted, StatusCode::kOk);
}

Result<GrantResponse> DaemonCore::renew(const RenewRequest& request) {
    const std::lock_guard<std::mutex> guard(mutex_);
    const TimestampMs now = now_ms();
    const Status ready = ensure_snapshot_locked();
    if (!ready.ok()) {
        return ready;
    }
    const std::uint64_t ttl = request.ttl_ms == 0 ? config_.default_ttl_ms : request.ttl_ms;
    const Result<Grant> renewed = grants_.renew(snapshot_, request.token, ttl, now);
    if (!renewed.has_value()) {
        GrantResponse response;
        response.code = renewed.error().code();
        response.detail = renewed.error().detail();
        return response;
    }
    StoreRecord commit;
    commit.type = RecordType::kGrantCommit;
    commit.payload = *renewed;
    RNF_TRYV(append_locked(commit));
    (void)maybe_checkpoint_locked();
    return grant_to_response(*renewed, StatusCode::kOk);
}

Result<AckResponse> DaemonCore::release(const TokenMessage& request) {
    const std::lock_guard<std::mutex> guard(mutex_);
    const Status released = grants_.release(request.token, now_ms());
    if (!released.ok()) {
        AckResponse response;
        response.code = released.code();
        response.detail = released.detail();
        return response;
    }
    const Grant* grant = grants_.find(request.token.grant);
    StoreRecord record;
    record.type = RecordType::kGrantState;
    GrantStateRecord body;
    body.id = request.token.grant;
    body.state = GrantState::kReleased;
    body.reason = grant != nullptr ? grant->reason : std::string("released");
    record.payload = std::move(body);
    RNF_TRYV(append_locked(record));
    (void)maybe_checkpoint_locked();
    AckResponse response;
    response.code = StatusCode::kOk;
    return response;
}

Result<GrantResponse> DaemonCore::validate(const TokenMessage& request) {
    const std::lock_guard<std::mutex> guard(mutex_);
    const Status ready = ensure_snapshot_locked();
    if (!ready.ok()) {
        return ready;
    }
    const Result<Grant> checked = grants_.validate(snapshot_, request.token, now_ms());
    if (!checked.has_value()) {
        GrantResponse response;
        response.code = checked.error().code();
        response.detail = checked.error().detail();
        return response;
    }
    return grant_to_response(*checked, StatusCode::kOk);
}

Result<LookupResponse> DaemonCore::lookup(const LookupRequest& request) {
    const std::lock_guard<std::mutex> guard(mutex_);
    const Result<RequestOutcome> outcome = grants_.lookup_request(request.request);
    LookupResponse response;
    if (!outcome.has_value()) {
        response.code = StatusCode::kUnknown;
        response.remembered = false;
        response.detail = outcome.error().detail();
        return response;
    }
    response.code = StatusCode::kOk;
    response.remembered = true;
    response.outcome_code = outcome->code;
    response.grant = outcome->grant;
    response.fence = outcome->fence;
    response.incarnation = outcome->incarnation;
    response.detail = outcome->detail;
    return response;
}

Result<StateResponse> DaemonCore::query_state() {
    const std::lock_guard<std::mutex> guard(mutex_);
    const Status ready = ensure_snapshot_locked();
    if (!ready.ok()) {
        return ready;
    }
    StateResponse response;
    response.code = StatusCode::kOk;
    response.rack = config_.rack.rack;
    response.generation = snapshot_.generation();
    response.epoch = snapshot_.epoch();
    response.incarnation = incarnation_;
    response.lifecycle = lifecycle_;
    response.snapshot_digest = snapshot_.digest();
    response.evidence_digest = snapshot_->evidence_digest;
    response.member_set_digest = snapshot_->member_set_digest;
    response.capacity.total = snapshot_->capacity.total;
    response.capacity.unavailable = snapshot_->capacity.unavailable;
    response.capacity.usable = snapshot_->capacity.usable;
    response.capacity.obligated = snapshot_->capacity.obligated;
    response.capacity.headroom_floor = snapshot_->capacity.headroom_floor;
    response.capacity.uncommitted = snapshot_->capacity.uncommitted;
    response.capacity.deficit = snapshot_->capacity.deficit;
    response.capacity.committed_grants = grants_.committed_total();
    response.devices = static_cast<std::uint32_t>(snapshot_->devices.size());
    response.ports = static_cast<std::uint32_t>(snapshot_->ports.size());
    response.links = static_cast<std::uint32_t>(snapshot_->links.size());
    response.attachments = static_cast<std::uint32_t>(snapshot_->attachments.size());
    response.paths = static_cast<std::uint32_t>(snapshot_->paths.size());
    response.live_grants = static_cast<std::uint32_t>(grants_.live_count());
    response.recovering_grants = static_cast<std::uint32_t>(grants_.recovering_count());
    response.total_diagnostics = static_cast<std::uint32_t>(snapshot_->diagnostics.size());
    const std::size_t shown = std::min(snapshot_->diagnostics.size(), kMaxStateDiagnostics);
    response.diagnostics.reserve(shown);
    for (std::size_t i = 0; i < shown; ++i) {
        const Diagnostic& source = snapshot_->diagnostics[i];
        DiagnosticEntry entry;
        entry.kind = source.kind;
        entry.subject_kind = source.subject_kind;
        entry.subject_id = source.subject_id;
        entry.source = source.source;
        entry.detail = source.detail;
        response.diagnostics.push_back(std::move(entry));
    }
    if (store_) {
        response.clean_shutdown = store_->recovery().clean_shutdown;
        response.recovered_from_dirty_shutdown = dirty_recovery_;
    }
    return response;
}

Result<ResourcesResponse> DaemonCore::query_resources(const ResourceQuery& request) {
    const std::lock_guard<std::mutex> guard(mutex_);
    const Status ready = ensure_snapshot_locked();
    if (!ready.ok()) {
        return ready;
    }
    ResourcesResponse response;
    response.code = StatusCode::kOk;
    const std::size_t limit = request.limit == 0 ? 256U : request.limit;
    for (const ResourceState& state : snapshot_->resources) {
        if (request.kind_filter != 0 &&
            static_cast<std::uint8_t>(state.ref.kind) != request.kind_filter) {
            continue;
        }
        if (response.resources.size() >= limit) {
            response.truncated = true;
            break;
        }
        ResourceEntry entry;
        entry.ref = state.ref;
        entry.eligibility = state.eligibility;
        entry.availability = state.availability;
        entry.capacity_known = state.capacity_known;
        entry.capacity = state.capacity;
        entry.obligated = state.obligated;
        entry.maintenance = state.maintenance;
        const Result<Capacity> committed = grants_.committed(state.ref);
        entry.committed = committed.has_value() ? *committed : Capacity{};
        const Result<Capacity> free_capacity = grants_.available(snapshot_, state.ref);
        if (free_capacity.has_value()) {
            entry.available = *free_capacity;
            entry.available_known = true;
            entry.available_code = StatusCode::kOk;
        } else {
            entry.available_known = false;
            entry.available_code = free_capacity.error().code();
        }
        response.resources.push_back(std::move(entry));
    }
    return response;
}

Result<PathsResponse> DaemonCore::query_paths() {
    const std::lock_guard<std::mutex> guard(mutex_);
    const Status ready = ensure_snapshot_locked();
    if (!ready.ok()) {
        return ready;
    }
    PathsResponse response;
    response.code = StatusCode::kOk;
    response.paths.reserve(snapshot_->paths.size());
    for (const PathRecord& path : snapshot_->paths) {
        PathEntry entry;
        entry.id = path.id;
        entry.start_port = path.start_port;
        entry.end_port = path.end_port;
        entry.hops = static_cast<std::uint32_t>(path.links.size());
        entry.bottleneck = path.bottleneck;
        entry.bottleneck_known = path.bottleneck_known;
        entry.eligibility = path.eligibility;
        response.paths.push_back(std::move(entry));
    }
    return response;
}

Result<GrantsResponse> DaemonCore::query_grants() {
    const std::lock_guard<std::mutex> guard(mutex_);
    GrantsResponse response;
    response.code = StatusCode::kOk;
    const std::vector<Grant> grants = grants_.list();
    response.grants.reserve(grants.size());
    for (const Grant& grant : grants) {
        response.grants.push_back(GrantSummary::from(grant));
    }
    return response;
}

Status DaemonCore::transition_lifecycle_locked(LifecycleState target, std::string& detail) {
    if (!lifecycle_transition_allowed(lifecycle_, target)) {
        detail = std::string("the rack cannot move from ") + std::string(to_string(lifecycle_)) +
                 " to " + std::string(to_string(target));
        return Status(StatusCode::kInvalidLifecycleTransition, detail);
    }
    const TimestampMs now = now_ms();
    const LifecycleState previous = lifecycle_;
    lifecycle_ = target;
    if (target == LifecycleState::kMaintenance) {
        grants_.suspend_all("the rack entered a maintenance window");
    }
    if (target == LifecycleState::kRetired) {
        grants_.fence_all("the rack was retired");
    }
    if (target == LifecycleState::kActive || target == LifecycleState::kDegraded) {
        RNF_TRYV(compose_locked(generation_));
        if (previous == LifecycleState::kRecovering) {
            const Result<std::size_t> rearmed = grants_.revalidate(snapshot_, incarnation_, now);
            if (!rearmed.has_value()) {
                return rearmed.error();
            }
        }
        (void)grants_.resume_all(snapshot_, now);
    }
    StoreRecord record;
    record.type = RecordType::kLifecycle;
    LifecycleRecord body;
    body.state = target;
    body.generation = generation_;
    body.epoch = epoch_;
    record.payload = body;
    RNF_TRYV(append_locked(record));
    (void)maybe_checkpoint_locked();
    detail = std::string("the rack moved from ") + std::string(to_string(previous)) + " to " +
             std::string(to_string(target));
    return Status{};
}

Result<LifecycleResponse> DaemonCore::set_lifecycle(const LifecycleRequest& request) {
    const std::lock_guard<std::mutex> guard(mutex_);
    LifecycleResponse response;
    std::string detail;
    const Status transitioned = transition_lifecycle_locked(request.target, detail);
    response.lifecycle = lifecycle_;
    response.detail = detail;
    if (!transitioned.ok()) {
        response.code = transitioned.code();
        return response;
    }
    response.code = StatusCode::kOk;
    return response;
}

Result<AckResponse> DaemonCore::checkpoint() {
    const std::lock_guard<std::mutex> guard(mutex_);
    AckResponse response;
    if (!store_) {
        response.code = StatusCode::kUnsupported;
        response.detail = "this daemon has no durable store";
        return response;
    }
    const Status ready = ensure_snapshot_locked();
    if (!ready.ok()) {
        response.code = ready.code();
        response.detail = ready.detail();
        return response;
    }
    const Result<std::vector<std::uint8_t>> encoded = snapshot_.encode();
    if (!encoded.has_value()) {
        response.code = encoded.error().code();
        response.detail = encoded.error().detail();
        return response;
    }
    Checkpoint checkpoint;
    checkpoint.wal_offset = store_->log_bytes();
    checkpoint.generation = snapshot_.generation();
    checkpoint.epoch = snapshot_.epoch();
    checkpoint.lifecycle = lifecycle_;
    checkpoint.member_set_digest = member_set_digest_;
    checkpoint.incarnation = incarnation_;
    checkpoint.snapshot_digest = snapshot_.digest();
    checkpoint.ledger_digest = ledger_.digest();
    checkpoint.snapshot_bytes = *encoded;
    checkpoint.ledger = ledger_.records();
    const Status written = store_->write_checkpoint(checkpoint);
    if (!written.ok()) {
        response.code = written.code();
        response.detail = written.detail();
        return response;
    }
    ++stats_.checkpoints;
    response.code = StatusCode::kOk;
    response.detail = "checkpoint written at log offset " +
                      std::to_string(checkpoint.wal_offset);
    return response;
}

Result<StatsResponse> DaemonCore::stats() {
    const std::lock_guard<std::mutex> guard(mutex_);
    StatsResponse response;
    response.code = StatusCode::kOk;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time_);
    response.uptime_ms = static_cast<std::uint64_t>(elapsed.count());
    response.evidence_records = ledger_.size();
    response.evidence_sources = ledger_.source_count();
    response.evidence_conflicts = ledger_.conflict_count();
    response.evidence_superseded = ledger_.superseded_count();
    response.log_records = stats_.log_records;
    response.log_bytes = stats_.log_bytes;
    response.checkpoints = stats_.checkpoints;
    response.requests_served = stats_.requests_served;
    response.connections_accepted = stats_.connections_accepted;
    response.connections_rejected = stats_.connections_rejected;
    response.composes = stats_.composes;
    response.grants_issued = stats_.grants_issued;
    response.grants_refused = stats_.grants_refused;
    response.grants_fenced = stats_.grants_fenced;
    const SocketCounters counters = socket_counters();
    response.frames_rejected = counters.frames_rejected;
    response.bytes_sent = counters.bytes_sent;
    response.bytes_received = counters.bytes_received;
    return response;
}

void DaemonCore::note_connection_accepted() {
    const std::lock_guard<std::mutex> guard(mutex_);
    ++stats_.connections_accepted;
}

void DaemonCore::note_connection_rejected() {
    const std::lock_guard<std::mutex> guard(mutex_);
    ++stats_.connections_rejected;
}

void DaemonCore::note_frame_rejected() {
    const std::lock_guard<std::mutex> guard(mutex_);
    ++stats_.frames_rejected;
}

void DaemonCore::note_request_served() {
    const std::lock_guard<std::mutex> guard(mutex_);
    ++stats_.requests_served;
}

}  // namespace rnf




