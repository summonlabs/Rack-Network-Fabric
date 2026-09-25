// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/net/protocol.hpp"

#include <string>

namespace rnf {
namespace {

const ProtocolLimits kLimits{};

[[nodiscard]] Status finish(const ByteWriter& writer) {
    if (!writer.ok()) {
        return Status(StatusCode::kOversizeField, "message exceeds its encoding bound");
    }
    return Status{};
}

[[nodiscard]] Status trailing(const ByteReader& reader) {
    if (!reader.at_end()) {
        return Status(StatusCode::kMalformedEncoding, "message has trailing bytes");
    }
    return Status{};
}

[[nodiscard]] Status truncated_error(std::string_view what) {
    return Status(StatusCode::kTruncated, std::string(what) + " is truncated");
}

}  // namespace

const ProtocolLimits& protocol_limits() noexcept {
    return kLimits;
}

std::string_view to_string(MessageType type) noexcept {
    switch (type) {
        case MessageType::kHello: return "hello";
        case MessageType::kHelloAck: return "hello_ack";
        case MessageType::kSubmitEvidence: return "submit_evidence";
        case MessageType::kEvidenceAck: return "evidence_ack";
        case MessageType::kCompose: return "compose";
        case MessageType::kComposeAck: return "compose_ack";
        case MessageType::kAcquire: return "acquire";
        case MessageType::kAcquireAck: return "acquire_ack";
        case MessageType::kRenew: return "renew";
        case MessageType::kRenewAck: return "renew_ack";
        case MessageType::kRelease: return "release";
        case MessageType::kReleaseAck: return "release_ack";
        case MessageType::kValidate: return "validate";
        case MessageType::kValidateAck: return "validate_ack";
        case MessageType::kLookup: return "lookup";
        case MessageType::kLookupAck: return "lookup_ack";
        case MessageType::kQueryState: return "query_state";
        case MessageType::kStateAck: return "state_ack";
        case MessageType::kQueryResources: return "query_resources";
        case MessageType::kResourcesAck: return "resources_ack";
        case MessageType::kQueryPaths: return "query_paths";
        case MessageType::kPathsAck: return "paths_ack";
        case MessageType::kQueryGrants: return "query_grants";
        case MessageType::kGrantsAck: return "grants_ack";
        case MessageType::kLifecycle: return "lifecycle";
        case MessageType::kLifecycleAck: return "lifecycle_ack";
        case MessageType::kCheckpoint: return "checkpoint";
        case MessageType::kCheckpointAck: return "checkpoint_ack";
        case MessageType::kStats: return "stats";
        case MessageType::kStatsAck: return "stats_ack";
        case MessageType::kShutdown: return "shutdown";
        case MessageType::kShutdownAck: return "shutdown_ack";
        case MessageType::kError: return "error";
    }
    return "unknown";
}

bool is_known_message_type(std::uint16_t type) noexcept {
    switch (static_cast<MessageType>(type)) {
        case MessageType::kHello:
        case MessageType::kHelloAck:
        case MessageType::kSubmitEvidence:
        case MessageType::kEvidenceAck:
        case MessageType::kCompose:
        case MessageType::kComposeAck:
        case MessageType::kAcquire:
        case MessageType::kAcquireAck:
        case MessageType::kRenew:
        case MessageType::kRenewAck:
        case MessageType::kRelease:
        case MessageType::kReleaseAck:
        case MessageType::kValidate:
        case MessageType::kValidateAck:
        case MessageType::kLookup:
        case MessageType::kLookupAck:
        case MessageType::kQueryState:
        case MessageType::kStateAck:
        case MessageType::kQueryResources:
        case MessageType::kResourcesAck:
        case MessageType::kQueryPaths:
        case MessageType::kPathsAck:
        case MessageType::kQueryGrants:
        case MessageType::kGrantsAck:
        case MessageType::kLifecycle:
        case MessageType::kLifecycleAck:
        case MessageType::kCheckpoint:
        case MessageType::kCheckpointAck:
        case MessageType::kStats:
        case MessageType::kStatsAck:
        case MessageType::kShutdown:
        case MessageType::kShutdownAck:
        case MessageType::kError:
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Small value helpers
// ---------------------------------------------------------------------------

namespace {

void put_ref(ByteWriter& writer, ResourceRef ref) {
    writer.u8(static_cast<std::uint8_t>(ref.kind));
    writer.u64(ref.id);
}

bool get_ref(ByteReader& reader, ResourceRef& ref) {
    std::uint8_t kind = 0;
    if (!reader.u8(kind) || !reader.u64(ref.id)) {
        return false;
    }
    ref.kind = static_cast<ResourceKind>(kind);
    return static_cast<std::uint8_t>(ref.kind) >= 1U &&
           static_cast<std::uint8_t>(ref.kind) <= 6U;
}

void put_capacity(ByteWriter& writer, Capacity value) {
    writer.u64(value.units);
}

}  // namespace

// ---------------------------------------------------------------------------
// Hello
// ---------------------------------------------------------------------------

Status HelloRequest::encode(ByteWriter& writer) const {
    writer.u16(protocol_version);
    writer.u32(client_id);
    return finish(writer);
}

Result<HelloRequest> HelloRequest::decode(ByteReader& reader) {
    HelloRequest message;
    if (!reader.u16(message.protocol_version) || !reader.u32(message.client_id)) {
        return truncated_error("hello request");
    }
    RNF_TRYV(trailing(reader));
    return message;
}

Status HelloResponse::encode(ByteWriter& writer) const {
    writer.u16(protocol_version);
    writer.boolean(accepted);
    writer.u64(rack.value());
    writer.u64(epoch.value);
    writer.u64(generation.value);
    writer.u64(incarnation.value);
    writer.u8(static_cast<std::uint8_t>(lifecycle));
    writer.digest(snapshot_digest);
    writer.text(detail, kLimits.max_detail_text);
    return finish(writer);
}

Result<HelloResponse> HelloResponse::decode(ByteReader& reader) {
    HelloResponse message;
    std::uint64_t rack = 0;
    std::uint64_t incarnation = 0;
    std::uint8_t lifecycle = 0;
    if (!reader.u16(message.protocol_version) || !reader.boolean(message.accepted) ||
        !reader.u64(rack) || !reader.u64(message.epoch.value) ||
        !reader.u64(message.generation.value) || !reader.u64(incarnation) ||
        !reader.u8(lifecycle) || !reader.digest(message.snapshot_digest) ||
        !reader.text(kLimits.max_detail_text, message.detail)) {
        return truncated_error("hello response");
    }
    message.rack = RackId(rack);
    message.incarnation = ControllerIncarnation{incarnation};
    message.lifecycle = static_cast<LifecycleState>(lifecycle);
    RNF_TRYV(trailing(reader));
    return message;
}

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

Status SubmitEvidenceRequest::encode(ByteWriter& writer) const {
    writer.u32(static_cast<std::uint32_t>(records.size()));
    for (const EvidenceRecord& record : records) {
        RNF_TRYV(::rnf::encode(record, writer));
    }
    return finish(writer);
}

Result<SubmitEvidenceRequest> SubmitEvidenceRequest::decode(ByteReader& reader) {
    SubmitEvidenceRequest message;
    std::uint32_t count = 0;
    if (!reader.count(count, kLimits.max_evidence_records)) {
        return Status(StatusCode::kOversizeField, "evidence batch exceeds its bound");
    }
    message.records.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        RNF_TRY(record, decode_evidence(reader));
        message.records.push_back(std::move(record));
    }
    RNF_TRYV(trailing(reader));
    return message;
}

Status EvidenceAckResponse::encode(ByteWriter& writer) const {
    writer.u32(static_cast<std::uint32_t>(entries.size()));
    for (const EvidenceAckEntry& entry : entries) {
        writer.u8(static_cast<std::uint8_t>(entry.outcome));
        writer.u16(static_cast<std::uint16_t>(entry.code));
        writer.digest(entry.digest);
        writer.text(entry.detail, kLimits.max_detail_text);
    }
    return finish(writer);
}

Result<EvidenceAckResponse> EvidenceAckResponse::decode(ByteReader& reader) {
    EvidenceAckResponse message;
    std::uint32_t count = 0;
    if (!reader.count(count, kLimits.max_evidence_records)) {
        return Status(StatusCode::kOversizeField, "evidence acknowledgement exceeds its bound");
    }
    message.entries.resize(count);
    for (EvidenceAckEntry& entry : message.entries) {
        std::uint8_t outcome = 0;
        std::uint16_t code = 0;
        if (!reader.u8(outcome) || !reader.u16(code) || !reader.digest(entry.digest) ||
            !reader.text(kLimits.max_detail_text, entry.detail)) {
            return truncated_error("evidence acknowledgement");
        }
        entry.outcome = static_cast<InsertOutcome>(outcome);
        entry.code = static_cast<StatusCode>(code);
    }
    RNF_TRYV(trailing(reader));
    return message;
}

// ---------------------------------------------------------------------------
// Compose
// ---------------------------------------------------------------------------

Status ComposeCommand::encode(ByteWriter& writer) const {
    writer.u64(generation.value);
    return finish(writer);
}

Result<ComposeCommand> ComposeCommand::decode(ByteReader& reader) {
    ComposeCommand message;
    if (!reader.u64(message.generation.value)) {
        return truncated_error("compose request");
    }
    RNF_TRYV(trailing(reader));
    return message;
}

Status ComposeResponse::encode(ByteWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(code));
    writer.u64(generation.value);
    writer.u64(epoch.value);
    writer.u64(incarnation.value);
    writer.digest(digest);
    writer.u32(diagnostics);
    writer.text(detail, kLimits.max_detail_text);
    return finish(writer);
}

Result<ComposeResponse> ComposeResponse::decode(ByteReader& reader) {
    ComposeResponse message;
    std::uint16_t code = 0;
    std::uint64_t incarnation = 0;
    if (!reader.u16(code) || !reader.u64(message.generation.value) ||
        !reader.u64(message.epoch.value) || !reader.u64(incarnation) ||
        !reader.digest(message.digest) || !reader.u32(message.diagnostics) ||
        !reader.text(kLimits.max_detail_text, message.detail)) {
        return truncated_error("compose response");
    }
    message.code = static_cast<StatusCode>(code);
    message.incarnation = ControllerIncarnation{incarnation};
    RNF_TRYV(trailing(reader));
    return message;
}

// ---------------------------------------------------------------------------
// Grants
// ---------------------------------------------------------------------------

Status AcquireRequest::encode(ByteWriter& writer) const {
    writer.u64(request.hi);
    writer.u64(request.lo);
    writer.u64(principal.value());
    put_ref(writer, scope);
    writer.u8(static_cast<std::uint8_t>(mode));
    put_capacity(writer, capacity);
    writer.u64(ttl_ms);
    return finish(writer);
}

Result<AcquireRequest> AcquireRequest::decode(ByteReader& reader) {
    AcquireRequest message;
    std::uint64_t principal = 0;
    std::uint8_t mode = 0;
    if (!reader.u64(message.request.hi) || !reader.u64(message.request.lo) ||
        !reader.u64(principal) || !get_ref(reader, message.scope) || !reader.u8(mode) ||
        !reader.u64(message.capacity.units) || !reader.u64(message.ttl_ms)) {
        return truncated_error("acquire request");
    }
    message.principal = PrincipalId(principal);
    message.mode = static_cast<GrantMode>(mode);
    if (message.mode != GrantMode::kShared && message.mode != GrantMode::kExclusive) {
        return Status(StatusCode::kOutOfRange, "acquire request has an unknown grant mode");
    }
    RNF_TRYV(trailing(reader));
    return message;
}

GrantSummary GrantSummary::from(const Grant& grant) {
    GrantSummary summary;
    summary.id = grant.id;
    summary.request = grant.request;
    summary.principal = grant.principal;
    summary.scope = grant.scope;
    summary.mode = grant.mode;
    summary.capacity = grant.capacity;
    summary.generation = grant.generation;
    summary.epoch = grant.epoch;
    summary.incarnation = grant.incarnation;
    summary.fence = grant.fence;
    summary.issued_at_ms = grant.issued_at_ms;
    summary.expires_at_ms = grant.expires_at_ms;
    summary.state = grant.state;
    summary.basis = grant.authority_basis;
    summary.reason = grant.reason;
    return summary;
}

Status GrantSummary::encode(ByteWriter& writer) const {
    writer.u64(id.value());
    writer.u64(request.hi);
    writer.u64(request.lo);
    writer.u64(principal.value());
    put_ref(writer, scope);
    writer.u8(static_cast<std::uint8_t>(mode));
    put_capacity(writer, capacity);
    writer.u64(generation.value);
    writer.u64(epoch.value);
    writer.u64(incarnation.value);
    writer.u64(fence);
    writer.u64(issued_at_ms);
    writer.u64(expires_at_ms);
    writer.u8(static_cast<std::uint8_t>(state));
    writer.digest(basis);
    writer.text(reason, kLimits.max_detail_text);
    return finish(writer);
}

Result<GrantSummary> GrantSummary::decode(ByteReader& reader) {
    GrantSummary message;
    std::uint64_t id = 0;
    std::uint64_t principal = 0;
    std::uint64_t incarnation = 0;
    std::uint8_t mode = 0;
    std::uint8_t state = 0;
    if (!reader.u64(id) || !reader.u64(message.request.hi) || !reader.u64(message.request.lo) ||
        !reader.u64(principal) || !get_ref(reader, message.scope) || !reader.u8(mode) ||
        !reader.u64(message.capacity.units) || !reader.u64(message.generation.value) ||
        !reader.u64(message.epoch.value) || !reader.u64(incarnation) || !reader.u64(message.fence) ||
        !reader.u64(message.issued_at_ms) || !reader.u64(message.expires_at_ms) ||
        !reader.u8(state) || !reader.digest(message.basis) ||
        !reader.text(kLimits.max_detail_text, message.reason)) {
        return truncated_error("grant summary");
    }
    message.id = GrantId(id);
    message.principal = PrincipalId(principal);
    message.incarnation = ControllerIncarnation{incarnation};
    message.mode = static_cast<GrantMode>(mode);
    message.state = static_cast<GrantState>(state);
    return message;
}

Status GrantResponse::encode(ByteWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(code));
    writer.boolean(grant.id.is_set());
    if (grant.id.is_set()) {
        RNF_TRYV(grant.encode(writer));
    }
    writer.text(detail, kLimits.max_detail_text);
    return finish(writer);
}

Result<GrantResponse> GrantResponse::decode(ByteReader& reader) {
    GrantResponse message;
    std::uint16_t code = 0;
    bool present = false;
    if (!reader.u16(code) || !reader.boolean(present)) {
        return truncated_error("grant response");
    }
    message.code = static_cast<StatusCode>(code);
    if (present) {
        RNF_TRY(summary, GrantSummary::decode(reader));
        message.grant = std::move(summary);
    }
    if (!reader.text(kLimits.max_detail_text, message.detail)) {
        return truncated_error("grant response");
    }
    RNF_TRYV(trailing(reader));
    return message;
}

Status TokenMessage::encode(ByteWriter& writer) const {
    writer.u64(token.grant.value());
    writer.u64(token.fence);
    writer.u64(token.incarnation.value);
    writer.digest(token.scope_basis);
    return finish(writer);
}

Result<TokenMessage> TokenMessage::decode(ByteReader& reader, bool require_end) {
    TokenMessage message;
    std::uint64_t grant = 0;
    std::uint64_t incarnation = 0;
    if (!reader.u64(grant) || !reader.u64(message.token.fence) || !reader.u64(incarnation) ||
        !reader.digest(message.token.scope_basis)) {
        return truncated_error("lease token");
    }
    message.token.grant = GrantId(grant);
    message.token.incarnation = ControllerIncarnation{incarnation};
    if (require_end) {
        RNF_TRYV(trailing(reader));
    }
    return message;
}

Status RenewRequest::encode(ByteWriter& writer) const {
    TokenMessage wrapper;
    wrapper.token = token;
    RNF_TRYV(wrapper.encode(writer));
    writer.u64(ttl_ms);
    return finish(writer);
}

Result<RenewRequest> RenewRequest::decode(ByteReader& reader) {
    RenewRequest message;
    // The token is a prefix of this body, so the nested decoder must not
    // require the reader to be exhausted.
    RNF_TRY(token, TokenMessage::decode(reader, false));
    message.token = token.token;
    if (!reader.u64(message.ttl_ms)) {
        return truncated_error("renew request");
    }
    RNF_TRYV(trailing(reader));
    return message;
}

Status LookupRequest::encode(ByteWriter& writer) const {
    writer.u64(request.hi);
    writer.u64(request.lo);
    return finish(writer);
}

Result<LookupRequest> LookupRequest::decode(ByteReader& reader) {
    LookupRequest message;
    if (!reader.u64(message.request.hi) || !reader.u64(message.request.lo)) {
        return truncated_error("lookup request");
    }
    RNF_TRYV(trailing(reader));
    return message;
}

Status LookupResponse::encode(ByteWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(code));
    writer.boolean(remembered);
    writer.u16(static_cast<std::uint16_t>(outcome_code));
    writer.u64(grant.value());
    writer.u64(fence);
    writer.u64(incarnation.value);
    writer.text(detail, kLimits.max_detail_text);
    return finish(writer);
}

Result<LookupResponse> LookupResponse::decode(ByteReader& reader) {
    LookupResponse message;
    std::uint16_t code = 0;
    std::uint16_t outcome = 0;
    std::uint64_t grant = 0;
    std::uint64_t incarnation = 0;
    if (!reader.u16(code) || !reader.boolean(message.remembered) || !reader.u16(outcome) ||
        !reader.u64(grant) || !reader.u64(message.fence) || !reader.u64(incarnation) ||
        !reader.text(kLimits.max_detail_text, message.detail)) {
        return truncated_error("lookup response");
    }
    message.code = static_cast<StatusCode>(code);
    message.outcome_code = static_cast<StatusCode>(outcome);
    message.grant = GrantId(grant);
    message.incarnation = ControllerIncarnation{incarnation};
    RNF_TRYV(trailing(reader));
    return message;
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

Status DiagnosticEntry::encode(ByteWriter& writer) const {
    writer.u8(static_cast<std::uint8_t>(kind));
    writer.u8(static_cast<std::uint8_t>(subject_kind));
    writer.u64(subject_id);
    writer.u64(source.value());
    writer.text(detail, kLimits.max_detail_text);
    return finish(writer);
}

Result<DiagnosticEntry> DiagnosticEntry::decode(ByteReader& reader) {
    DiagnosticEntry message;
    std::uint8_t kind = 0;
    std::uint8_t subject_kind = 0;
    std::uint64_t source = 0;
    if (!reader.u8(kind) || !reader.u8(subject_kind) || !reader.u64(message.subject_id) ||
        !reader.u64(source) || !reader.text(kLimits.max_detail_text, message.detail)) {
        return truncated_error("diagnostic entry");
    }
    message.kind = static_cast<DiagnosticKind>(kind);
    message.subject_kind = static_cast<EvidenceKind>(subject_kind);
    message.source = SourceId(source);
    return message;
}

bool CapacityMessage::closes() const noexcept {
    std::uint64_t lhs = 0;
    if (!checked_add_u64(unavailable.units, usable.units, lhs) || lhs != total.units) {
        return false;
    }
    std::uint64_t rhs = 0;
    if (!checked_add_u64(obligated.units, headroom_floor.units, rhs) ||
        !checked_add_u64(rhs, uncommitted.units, rhs)) {
        return false;
    }
    std::uint64_t lhs2 = 0;
    return checked_add_u64(usable.units, deficit.units, lhs2) && lhs2 == rhs;
}

Status CapacityMessage::encode(ByteWriter& writer) const {
    put_capacity(writer, total);
    put_capacity(writer, unavailable);
    put_capacity(writer, usable);
    put_capacity(writer, obligated);
    put_capacity(writer, headroom_floor);
    put_capacity(writer, uncommitted);
    put_capacity(writer, deficit);
    put_capacity(writer, committed_grants);
    return finish(writer);
}

Result<CapacityMessage> CapacityMessage::decode(ByteReader& reader) {
    CapacityMessage message;
    if (!reader.u64(message.total.units) || !reader.u64(message.unavailable.units) ||
        !reader.u64(message.usable.units) || !reader.u64(message.obligated.units) ||
        !reader.u64(message.headroom_floor.units) || !reader.u64(message.uncommitted.units) ||
        !reader.u64(message.deficit.units) || !reader.u64(message.committed_grants.units)) {
        return truncated_error("capacity message");
    }
    return message;
}

Status StateResponse::encode(ByteWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(code));
    writer.u64(rack.value());
    writer.u64(generation.value);
    writer.u64(epoch.value);
    writer.u64(incarnation.value);
    writer.u8(static_cast<std::uint8_t>(lifecycle));
    writer.digest(snapshot_digest);
    writer.digest(evidence_digest);
    writer.digest(member_set_digest);
    RNF_TRYV(capacity.encode(writer));
    writer.u32(devices);
    writer.u32(ports);
    writer.u32(links);
    writer.u32(attachments);
    writer.u32(paths);
    writer.u32(live_grants);
    writer.u32(recovering_grants);
    writer.boolean(clean_shutdown);
    writer.boolean(recovered_from_dirty_shutdown);
    writer.u32(total_diagnostics);
    writer.u32(static_cast<std::uint32_t>(diagnostics.size()));
    for (const DiagnosticEntry& entry : diagnostics) {
        RNF_TRYV(entry.encode(writer));
    }
    writer.text(detail, kLimits.max_detail_text);
    return finish(writer);
}

Result<StateResponse> StateResponse::decode(ByteReader& reader) {
    StateResponse message;
    std::uint16_t code = 0;
    std::uint64_t rack = 0;
    std::uint64_t incarnation = 0;
    std::uint8_t lifecycle = 0;
    if (!reader.u16(code) || !reader.u64(rack) || !reader.u64(message.generation.value) ||
        !reader.u64(message.epoch.value) || !reader.u64(incarnation) || !reader.u8(lifecycle) ||
        !reader.digest(message.snapshot_digest) || !reader.digest(message.evidence_digest) ||
        !reader.digest(message.member_set_digest)) {
        return truncated_error("state response");
    }
    message.code = static_cast<StatusCode>(code);
    message.rack = RackId(rack);
    message.incarnation = ControllerIncarnation{incarnation};
    message.lifecycle = static_cast<LifecycleState>(lifecycle);
    RNF_TRY(capacity, CapacityMessage::decode(reader));
    message.capacity = capacity;
    if (!reader.u32(message.devices) || !reader.u32(message.ports) || !reader.u32(message.links) ||
        !reader.u32(message.attachments) || !reader.u32(message.paths) ||
        !reader.u32(message.live_grants) || !reader.u32(message.recovering_grants) ||
        !reader.boolean(message.clean_shutdown) ||
        !reader.boolean(message.recovered_from_dirty_shutdown) ||
        !reader.u32(message.total_diagnostics)) {
        return truncated_error("state response");
    }
    std::uint32_t count = 0;
    if (!reader.count(count, kLimits.max_diagnostics)) {
        return Status(StatusCode::kOversizeField, "diagnostic list exceeds its bound");
    }
    message.diagnostics.resize(count);
    for (DiagnosticEntry& entry : message.diagnostics) {
        RNF_TRY(decoded, DiagnosticEntry::decode(reader));
        entry = std::move(decoded);
    }
    if (!reader.text(kLimits.max_detail_text, message.detail)) {
        return truncated_error("state response");
    }
    RNF_TRYV(trailing(reader));
    return message;
}

Status ResourceQuery::encode(ByteWriter& writer) const {
    writer.u32(limit);
    writer.u8(kind_filter);
    return finish(writer);
}

Result<ResourceQuery> ResourceQuery::decode(ByteReader& reader) {
    ResourceQuery message;
    if (!reader.u32(message.limit) || !reader.u8(message.kind_filter)) {
        return truncated_error("resource query");
    }
    RNF_TRYV(trailing(reader));
    return message;
}

Status ResourceEntry::encode(ByteWriter& writer) const {
    put_ref(writer, ref);
    writer.u8(static_cast<std::uint8_t>(eligibility));
    writer.u8(static_cast<std::uint8_t>(availability));
    writer.boolean(capacity_known);
    put_capacity(writer, capacity);
    put_capacity(writer, obligated);
    put_capacity(writer, committed);
    put_capacity(writer, available);
    writer.boolean(available_known);
    writer.u16(static_cast<std::uint16_t>(available_code));
    writer.boolean(maintenance);
    return finish(writer);
}

Result<ResourceEntry> ResourceEntry::decode(ByteReader& reader) {
    ResourceEntry message;
    std::uint8_t eligibility = 0;
    std::uint8_t availability = 0;
    std::uint16_t code = 0;
    if (!get_ref(reader, message.ref) || !reader.u8(eligibility) || !reader.u8(availability) ||
        !reader.boolean(message.capacity_known) || !reader.u64(message.capacity.units) ||
        !reader.u64(message.obligated.units) || !reader.u64(message.committed.units) ||
        !reader.u64(message.available.units) || !reader.boolean(message.available_known) ||
        !reader.u16(code) || !reader.boolean(message.maintenance)) {
        return truncated_error("resource entry");
    }
    message.eligibility = static_cast<Eligibility>(eligibility);
    message.availability = static_cast<Availability>(availability);
    message.available_code = static_cast<StatusCode>(code);
    return message;
}

Status ResourcesResponse::encode(ByteWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(code));
    writer.boolean(truncated);
    writer.u32(static_cast<std::uint32_t>(resources.size()));
    for (const ResourceEntry& entry : resources) {
        RNF_TRYV(entry.encode(writer));
    }
    writer.text(detail, kLimits.max_detail_text);
    return finish(writer);
}

Result<ResourcesResponse> ResourcesResponse::decode(ByteReader& reader) {
    ResourcesResponse message;
    std::uint16_t code = 0;
    if (!reader.u16(code) || !reader.boolean(message.truncated)) {
        return truncated_error("resources response");
    }
    message.code = static_cast<StatusCode>(code);
    std::uint32_t count = 0;
    if (!reader.count(count, kLimits.max_resources)) {
        return Status(StatusCode::kOversizeField, "resource list exceeds its bound");
    }
    message.resources.resize(count);
    for (ResourceEntry& entry : message.resources) {
        RNF_TRY(decoded, ResourceEntry::decode(reader));
        entry = std::move(decoded);
    }
    if (!reader.text(kLimits.max_detail_text, message.detail)) {
        return truncated_error("resources response");
    }
    RNF_TRYV(trailing(reader));
    return message;
}

Status PathEntry::encode(ByteWriter& writer) const {
    writer.u64(id.value());
    writer.u64(start_port.value());
    writer.u64(end_port.value());
    writer.u32(hops);
    put_capacity(writer, bottleneck);
    writer.boolean(bottleneck_known);
    writer.u8(static_cast<std::uint8_t>(eligibility));
    return finish(writer);
}

Result<PathEntry> PathEntry::decode(ByteReader& reader) {
    PathEntry message;
    std::uint64_t id = 0;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint8_t eligibility = 0;
    if (!reader.u64(id) || !reader.u64(start) || !reader.u64(end) || !reader.u32(message.hops) ||
        !reader.u64(message.bottleneck.units) || !reader.boolean(message.bottleneck_known) ||
        !reader.u8(eligibility)) {
        return truncated_error("path entry");
    }
    message.id = PathId(id);
    message.start_port = PortId(start);
    message.end_port = PortId(end);
    message.eligibility = static_cast<Eligibility>(eligibility);
    return message;
}

Status PathsResponse::encode(ByteWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(code));
    writer.boolean(truncated);
    writer.u32(static_cast<std::uint32_t>(paths.size()));
    for (const PathEntry& entry : paths) {
        RNF_TRYV(entry.encode(writer));
    }
    writer.text(detail, kLimits.max_detail_text);
    return finish(writer);
}

Result<PathsResponse> PathsResponse::decode(ByteReader& reader) {
    PathsResponse message;
    std::uint16_t code = 0;
    if (!reader.u16(code) || !reader.boolean(message.truncated)) {
        return truncated_error("paths response");
    }
    message.code = static_cast<StatusCode>(code);
    std::uint32_t count = 0;
    if (!reader.count(count, kLimits.max_paths)) {
        return Status(StatusCode::kOversizeField, "path list exceeds its bound");
    }
    message.paths.resize(count);
    for (PathEntry& entry : message.paths) {
        RNF_TRY(decoded, PathEntry::decode(reader));
        entry = std::move(decoded);
    }
    if (!reader.text(kLimits.max_detail_text, message.detail)) {
        return truncated_error("paths response");
    }
    RNF_TRYV(trailing(reader));
    return message;
}

Status GrantsResponse::encode(ByteWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(code));
    writer.boolean(truncated);
    writer.u32(static_cast<std::uint32_t>(grants.size()));
    for (const GrantSummary& entry : grants) {
        RNF_TRYV(entry.encode(writer));
    }
    writer.text(detail, kLimits.max_detail_text);
    return finish(writer);
}

Result<GrantsResponse> GrantsResponse::decode(ByteReader& reader) {
    GrantsResponse message;
    std::uint16_t code = 0;
    if (!reader.u16(code) || !reader.boolean(message.truncated)) {
        return truncated_error("grants response");
    }
    message.code = static_cast<StatusCode>(code);
    std::uint32_t count = 0;
    if (!reader.count(count, kLimits.max_grants)) {
        return Status(StatusCode::kOversizeField, "grant list exceeds its bound");
    }
    message.grants.resize(count);
    for (GrantSummary& entry : message.grants) {
        RNF_TRY(decoded, GrantSummary::decode(reader));
        entry = std::move(decoded);
    }
    if (!reader.text(kLimits.max_detail_text, message.detail)) {
        return truncated_error("grants response");
    }
    RNF_TRYV(trailing(reader));
    return message;
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

Status LifecycleRequest::encode(ByteWriter& writer) const {
    writer.u8(static_cast<std::uint8_t>(target));
    return finish(writer);
}

Result<LifecycleRequest> LifecycleRequest::decode(ByteReader& reader) {
    LifecycleRequest message;
    std::uint8_t target = 0;
    if (!reader.u8(target)) {
        return truncated_error("lifecycle request");
    }
    message.target = static_cast<LifecycleState>(target);
    if (static_cast<std::uint8_t>(message.target) < 1U ||
        static_cast<std::uint8_t>(message.target) > 7U) {
        return Status(StatusCode::kOutOfRange, "lifecycle request names an unknown state");
    }
    RNF_TRYV(trailing(reader));
    return message;
}

Status LifecycleResponse::encode(ByteWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(code));
    writer.u8(static_cast<std::uint8_t>(lifecycle));
    writer.text(detail, kLimits.max_detail_text);
    return finish(writer);
}

Result<LifecycleResponse> LifecycleResponse::decode(ByteReader& reader) {
    LifecycleResponse message;
    std::uint16_t code = 0;
    std::uint8_t lifecycle = 0;
    if (!reader.u16(code) || !reader.u8(lifecycle) ||
        !reader.text(kLimits.max_detail_text, message.detail)) {
        return truncated_error("lifecycle response");
    }
    message.code = static_cast<StatusCode>(code);
    message.lifecycle = static_cast<LifecycleState>(lifecycle);
    RNF_TRYV(trailing(reader));
    return message;
}

Status StatsResponse::encode(ByteWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(code));
    writer.u64(uptime_ms);
    writer.u64(evidence_records);
    writer.u64(evidence_sources);
    writer.u64(evidence_conflicts);
    writer.u64(evidence_superseded);
    writer.u64(log_records);
    writer.u64(log_bytes);
    writer.u64(checkpoints);
    writer.u64(requests_served);
    writer.u64(connections_accepted);
    writer.u64(connections_rejected);
    writer.u64(frames_rejected);
    writer.u64(bytes_sent);
    writer.u64(bytes_received);
    writer.u64(composes);
    writer.u64(grants_issued);
    writer.u64(grants_refused);
    writer.u64(grants_fenced);
    writer.text(detail, kLimits.max_detail_text);
    return finish(writer);
}

Result<StatsResponse> StatsResponse::decode(ByteReader& reader) {
    StatsResponse message;
    std::uint16_t code = 0;
    if (!reader.u16(code) || !reader.u64(message.uptime_ms) ||
        !reader.u64(message.evidence_records) || !reader.u64(message.evidence_sources) ||
        !reader.u64(message.evidence_conflicts) || !reader.u64(message.evidence_superseded) ||
        !reader.u64(message.log_records) || !reader.u64(message.log_bytes) ||
        !reader.u64(message.checkpoints) || !reader.u64(message.requests_served) ||
        !reader.u64(message.connections_accepted) ||
        !reader.u64(message.connections_rejected) || !reader.u64(message.frames_rejected) ||
        !reader.u64(message.bytes_sent) || !reader.u64(message.bytes_received) ||
        !reader.u64(message.composes) || !reader.u64(message.grants_issued) ||
        !reader.u64(message.grants_refused) || !reader.u64(message.grants_fenced) ||
        !reader.text(kLimits.max_detail_text, message.detail)) {
        return truncated_error("stats response");
    }
    message.code = static_cast<StatusCode>(code);
    RNF_TRYV(trailing(reader));
    return message;
}

Status AckResponse::encode(ByteWriter& writer) const {
    writer.u16(static_cast<std::uint16_t>(code));
    writer.text(detail, kLimits.max_detail_text);
    return finish(writer);
}

Result<AckResponse> AckResponse::decode(ByteReader& reader) {
    AckResponse message;
    std::uint16_t code = 0;
    if (!reader.u16(code) || !reader.text(kLimits.max_detail_text, message.detail)) {
        return truncated_error("acknowledgement");
    }
    message.code = static_cast<StatusCode>(code);
    RNF_TRYV(trailing(reader));
    return message;
}



}  // namespace rnf
