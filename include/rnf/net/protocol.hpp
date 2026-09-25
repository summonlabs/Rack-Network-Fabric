// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The rnfd wire protocol. One request produces exactly one response; the frame
// header carries the message type and a correlation identifier, and the body is
// the canonical encoding of the structure below.

#ifndef RNF_NET_PROTOCOL_HPP
#define RNF_NET_PROTOCOL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "rnf/authority/grant.hpp"
#include "rnf/compose/snapshot.hpp"
#include "rnf/core/bytes.hpp"
#include "rnf/model/evidence.hpp"
#include "rnf/net/frame.hpp"
#include "rnf/version.hpp"

namespace rnf {

enum class MessageType : std::uint16_t {
    kHello = 1,
    kHelloAck = 2,
    kSubmitEvidence = 16,
    kEvidenceAck = 17,
    kCompose = 32,
    kComposeAck = 33,
    kAcquire = 48,
    kAcquireAck = 49,
    kRenew = 50,
    kRenewAck = 51,
    kRelease = 52,
    kReleaseAck = 53,
    kValidate = 54,
    kValidateAck = 55,
    kLookup = 56,
    kLookupAck = 57,
    kQueryState = 64,
    kStateAck = 65,
    kQueryResources = 66,
    kResourcesAck = 67,
    kQueryPaths = 68,
    kPathsAck = 69,
    kQueryGrants = 70,
    kGrantsAck = 71,
    kLifecycle = 80,
    kLifecycleAck = 81,
    kCheckpoint = 82,
    kCheckpointAck = 83,
    kStats = 84,
    kStatsAck = 85,
    kShutdown = 86,
    kShutdownAck = 87,
    kError = 32512,  // 0x7F00
};

[[nodiscard]] std::string_view to_string(MessageType type) noexcept;

/// Ceilings applied when decoding a message body. A peer cannot make the
/// daemon allocate more than these, whatever it claims in a length prefix.
struct ProtocolLimits {
    std::uint32_t max_evidence_records = 4096;
    std::uint32_t max_resources = 65536;
    std::uint32_t max_paths = 8192;
    std::uint32_t max_grants = 8192;
    std::uint32_t max_diagnostics = 512;
    std::uint32_t max_detail_text = 512;
};

[[nodiscard]] const ProtocolLimits& protocol_limits() noexcept;

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

struct HelloRequest {
    std::uint16_t protocol_version = 0;
    std::uint32_t client_id = 0;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<HelloRequest> decode(ByteReader& reader);
};

struct HelloResponse {
    std::uint16_t protocol_version = 0;
    bool accepted = false;
    RackId rack{};
    RackEpoch epoch{};
    TopologyGeneration generation{};
    ControllerIncarnation incarnation{};
    LifecycleState lifecycle = LifecycleState::kAssembling;
    Digest snapshot_digest{};
    std::string detail;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<HelloResponse> decode(ByteReader& reader);
};

struct SubmitEvidenceRequest {
    std::vector<EvidenceRecord> records;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<SubmitEvidenceRequest> decode(ByteReader& reader);
};

struct EvidenceAckEntry {
    InsertOutcome outcome = InsertOutcome::kRefused;
    StatusCode code = StatusCode::kOk;
    Digest digest{};
    std::string detail;
};

struct EvidenceAckResponse {
    std::vector<EvidenceAckEntry> entries;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<EvidenceAckResponse> decode(ByteReader& reader);
};

struct ComposeCommand {
    TopologyGeneration generation{};
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<ComposeCommand> decode(ByteReader& reader);
};

struct ComposeResponse {
    StatusCode code = StatusCode::kOk;
    TopologyGeneration generation{};
    RackEpoch epoch{};
    ControllerIncarnation incarnation{};
    Digest digest{};
    std::uint32_t diagnostics = 0;
    std::string detail;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<ComposeResponse> decode(ByteReader& reader);
};

struct AcquireRequest {
    RequestId request{};
    PrincipalId principal{};
    ResourceRef scope{};
    GrantMode mode = GrantMode::kShared;
    Capacity capacity{};
    std::uint64_t ttl_ms = 60000;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<AcquireRequest> decode(ByteReader& reader);
};

/// Compact, wire friendly view of a grant.
struct GrantSummary {
    GrantId id{};
    RequestId request{};
    PrincipalId principal{};
    ResourceRef scope{};
    GrantMode mode = GrantMode::kShared;
    Capacity capacity{};
    TopologyGeneration generation{};
    RackEpoch epoch{};
    ControllerIncarnation incarnation{};
    FencingToken fence = 0;
    TimestampMs issued_at_ms = 0;
    TimestampMs expires_at_ms = 0;
    GrantState state = GrantState::kPending;
    Digest basis{};
    std::string reason;

    [[nodiscard]] static GrantSummary from(const Grant& grant);
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<GrantSummary> decode(ByteReader& reader);
};

struct GrantResponse {
    StatusCode code = StatusCode::kOk;
    GrantSummary grant;
    std::string detail;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<GrantResponse> decode(ByteReader& reader);
};

struct TokenMessage {
    LeaseToken token{};
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    /// require_end is false when this message is the prefix of a larger body
    /// (RenewRequest embeds it). Enforcing "no trailing bytes" unconditionally
    /// would make every enclosing message undecodable.
    [[nodiscard]] static Result<TokenMessage> decode(ByteReader& reader,
                                                     bool require_end = true);
};

struct RenewRequest {
    LeaseToken token{};
    std::uint64_t ttl_ms = 60000;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<RenewRequest> decode(ByteReader& reader);
};

struct LookupRequest {
    RequestId request{};
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<LookupRequest> decode(ByteReader& reader);
};

struct LookupResponse {
    StatusCode code = StatusCode::kOk;
    StatusCode outcome_code = StatusCode::kOk;
    GrantId grant{};
    FencingToken fence = 0;
    ControllerIncarnation incarnation{};
    std::string detail;
    /// True when this daemon has a durable record of the request at all.
    bool remembered = false;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<LookupResponse> decode(ByteReader& reader);
};

struct DiagnosticEntry {
    DiagnosticKind kind = DiagnosticKind::kStaleEvidence;
    EvidenceKind subject_kind = EvidenceKind::kMember;
    std::uint64_t subject_id = 0;
    SourceId source{};
    std::string detail;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<DiagnosticEntry> decode(ByteReader& reader);
};

struct CapacityMessage {
    Capacity total{};
    Capacity unavailable{};
    Capacity usable{};
    Capacity obligated{};
    Capacity headroom_floor{};
    Capacity uncommitted{};
    Capacity deficit{};
    Capacity committed_grants{};
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<CapacityMessage> decode(ByteReader& reader);

    /// Re-check the two ledger identities on the wire representation:
    ///   total == unavailable + usable
    ///   usable + deficit == obligated + headroom_floor + uncommitted
    [[nodiscard]] bool closes() const noexcept;
};

struct StateResponse {
    StatusCode code = StatusCode::kOk;
    RackId rack{};
    TopologyGeneration generation{};
    RackEpoch epoch{};
    ControllerIncarnation incarnation{};
    LifecycleState lifecycle = LifecycleState::kAssembling;
    Digest snapshot_digest{};
    Digest evidence_digest{};
    Digest member_set_digest{};
    CapacityMessage capacity;
    std::uint32_t devices = 0;
    std::uint32_t ports = 0;
    std::uint32_t links = 0;
    std::uint32_t attachments = 0;
    std::uint32_t paths = 0;
    std::uint32_t live_grants = 0;
    std::uint32_t recovering_grants = 0;
    bool clean_shutdown = false;
    bool recovered_from_dirty_shutdown = false;
    std::uint32_t total_diagnostics = 0;
    std::vector<DiagnosticEntry> diagnostics;
    std::string detail;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<StateResponse> decode(ByteReader& reader);
};

struct ResourceQuery {
    std::uint32_t limit = 256;
    std::uint8_t kind_filter = 0;  // 0 means every kind
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<ResourceQuery> decode(ByteReader& reader);
};

struct ResourceEntry {
    ResourceRef ref{};
    Eligibility eligibility = Eligibility::kUnknown;
    Availability availability = Availability::kUnknown;
    bool capacity_known = false;
    Capacity capacity{};
    Capacity obligated{};
    Capacity committed{};
    Capacity available{};
    /// False when the available figure could not be computed, in which case the
    /// refusal code says why.
    bool available_known = false;
    StatusCode available_code = StatusCode::kOk;
    bool maintenance = false;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<ResourceEntry> decode(ByteReader& reader);
};

struct ResourcesResponse {
    StatusCode code = StatusCode::kOk;
    std::vector<ResourceEntry> resources;
    bool truncated = false;
    std::string detail;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<ResourcesResponse> decode(ByteReader& reader);
};

struct PathEntry {
    PathId id{};
    PortId start_port{};
    PortId end_port{};
    std::uint32_t hops = 0;
    Capacity bottleneck{};
    bool bottleneck_known = false;
    Eligibility eligibility = Eligibility::kUnknown;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<PathEntry> decode(ByteReader& reader);
};

struct PathsResponse {
    StatusCode code = StatusCode::kOk;
    std::vector<PathEntry> paths;
    bool truncated = false;
    std::string detail;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<PathsResponse> decode(ByteReader& reader);
};

struct GrantsResponse {
    StatusCode code = StatusCode::kOk;
    std::vector<GrantSummary> grants;
    bool truncated = false;
    std::string detail;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<GrantsResponse> decode(ByteReader& reader);
};

struct LifecycleRequest {
    LifecycleState target = LifecycleState::kActive;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<LifecycleRequest> decode(ByteReader& reader);
};

struct LifecycleResponse {
    StatusCode code = StatusCode::kOk;
    LifecycleState lifecycle = LifecycleState::kAssembling;
    std::string detail;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<LifecycleResponse> decode(ByteReader& reader);
};

struct StatsResponse {
    StatusCode code = StatusCode::kOk;
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
    std::string detail;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<StatsResponse> decode(ByteReader& reader);
};

/// Generic acknowledgement used by release, checkpoint, shutdown and the
/// lifecycle-independent commands.
struct AckResponse {
    StatusCode code = StatusCode::kOk;
    std::string detail;
    [[nodiscard]] Status encode(ByteWriter& writer) const;
    [[nodiscard]] static Result<AckResponse> decode(ByteReader& reader);
};

[[nodiscard]] bool is_known_message_type(std::uint16_t type) noexcept;

}  // namespace rnf

#endif  // RNF_NET_PROTOCOL_HPP
