// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/model/evidence.hpp"

#include <tuple>

namespace rnf {
namespace {

const EvidenceLimits kLimits{};

/// Rank used to decide which of two records for the same slot wins.
/// Higher is newer. Arrival order plays no part.
constexpr std::tuple<std::uint64_t, std::uint64_t> rank_of(
    const EvidenceRecord& record) noexcept {
    return {record.generation.value, record.provenance.sequence};
}

Status encode_text(ByteWriter& writer, const std::string& value, std::size_t max_len) {
    const ByteSpan bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
    if (!is_valid_utf8(bytes)) {
        return Status(StatusCode::kInvalidUnicode, "text field is not well-formed UTF-8");
    }
    if (value.size() > max_len) {
        return Status(StatusCode::kOversizeField, "text field exceeds its configured bound");
    }
    writer.text(value, max_len);
    return Status{};
}

}  // namespace

const EvidenceLimits& default_evidence_limits() noexcept {
    return kLimits;
}

std::string EvidenceSlot::to_text() const {
    std::string out(to_string(kind));
    out.push_back(':');
    out += std::to_string(subject_a);
    out.push_back('/');
    out += std::to_string(subject_b);
    out.push_back('@');
    out += source.to_hex();
    return out;
}

EvidenceSlot slot_of(const EvidenceRecord& record) noexcept {
    EvidenceSlot slot;
    slot.kind = record.kind;
    slot.source = record.provenance.source;
    switch (record.kind) {
        case EvidenceKind::kMember: {
            const auto& claim = std::get<MemberClaim>(record.payload);
            slot.subject_a = claim.device.value();
            slot.subject_b = claim.rack.value();
            break;
        }
        case EvidenceKind::kDevice: {
            slot.subject_a = std::get<DeviceClaim>(record.payload).device.value();
            break;
        }
        case EvidenceKind::kPort: {
            slot.subject_a = std::get<PortClaim>(record.payload).port.value();
            break;
        }
        case EvidenceKind::kLink: {
            slot.subject_a = std::get<LinkClaim>(record.payload).link.value();
            break;
        }
        case EvidenceKind::kAttachment: {
            slot.subject_a = std::get<AttachmentClaim>(record.payload).attachment.value();
            break;
        }
        case EvidenceKind::kMaintenance: {
            const auto& claim = std::get<MaintenanceClaim>(record.payload);
            slot.subject_a = static_cast<std::uint64_t>(claim.target_kind);
            slot.subject_b = claim.target_id;
            break;
        }
        case EvidenceKind::kObligation: {
            const auto& claim = std::get<ObligationClaim>(record.payload);
            slot.subject_a = claim.obligation.value();
            slot.subject_b = claim.target_id;
            break;
        }
    }
    return slot;
}

Status encode(const EvidenceRecord& record, ByteWriter& writer) {
    writer.u8(static_cast<std::uint8_t>(record.kind));
    writer.u8(static_cast<std::uint8_t>(record.durability));
    writer.u64(record.generation.value);
    writer.u64(record.provenance.source.value());
    writer.u8(static_cast<std::uint8_t>(record.provenance.kind));
    writer.u64(record.provenance.sequence);
    writer.u64(record.provenance.observed_at_ms);

    const EvidenceLimits& limits = default_evidence_limits();
    switch (record.kind) {
        case EvidenceKind::kMember: {
            const auto& claim = std::get<MemberClaim>(record.payload);
            writer.u64(claim.rack.value());
            writer.u64(claim.device.value());
            writer.u64(claim.incarnation);
            break;
        }
        case EvidenceKind::kDevice: {
            const auto& claim = std::get<DeviceClaim>(record.payload);
            writer.u64(claim.device.value());
            writer.u8(static_cast<std::uint8_t>(claim.role));
            writer.u64(claim.capacity.units);
            writer.boolean(claim.capacity_known);
            writer.u8(static_cast<std::uint8_t>(claim.availability));
            break;
        }
        case EvidenceKind::kPort: {
            const auto& claim = std::get<PortClaim>(record.payload);
            writer.u64(claim.port.value());
            writer.u64(claim.device.value());
            writer.u16(claim.index);
            writer.u8(static_cast<std::uint8_t>(claim.role));
            writer.u64(claim.capacity.units);
            writer.boolean(claim.capacity_known);
            writer.u8(static_cast<std::uint8_t>(claim.admin));
            writer.u8(static_cast<std::uint8_t>(claim.availability));
            break;
        }
        case EvidenceKind::kLink: {
            const auto& claim = std::get<LinkClaim>(record.payload);
            writer.u64(claim.link.value());
            writer.u64(claim.port_a.value());
            writer.u64(claim.port_b.value());
            writer.u8(static_cast<std::uint8_t>(claim.kind));
            writer.u64(claim.capacity.units);
            writer.boolean(claim.capacity_known);
            writer.u8(static_cast<std::uint8_t>(claim.availability));
            break;
        }
        case EvidenceKind::kAttachment: {
            const auto& claim = std::get<AttachmentClaim>(record.payload);
            writer.u64(claim.attachment.value());
            writer.u64(claim.device.value());
            writer.u64(claim.port.value());
            RNF_TRYV(encode_text(writer, claim.host_key, limits.max_host_key));
            break;
        }
        case EvidenceKind::kMaintenance: {
            const auto& claim = std::get<MaintenanceClaim>(record.payload);
            writer.u8(static_cast<std::uint8_t>(claim.action));
            writer.u8(static_cast<std::uint8_t>(claim.target_kind));
            writer.u64(claim.target_id);
            RNF_TRYV(encode_text(writer, claim.reason, limits.max_reason));
            break;
        }
        case EvidenceKind::kObligation: {
            const auto& claim = std::get<ObligationClaim>(record.payload);
            writer.u64(claim.obligation.value());
            writer.u8(static_cast<std::uint8_t>(claim.target_kind));
            writer.u64(claim.target_id);
            writer.u64(claim.capacity.units);
            writer.u64(claim.holder.value());
            RNF_TRYV(encode_text(writer, claim.authority, limits.max_authority));
            break;
        }
    }
    if (!writer.ok()) {
        return Status(StatusCode::kOversizeField, "evidence record exceeds its encoding bound");
    }
    return Status{};
}

Result<EvidenceRecord> decode_evidence(ByteReader& reader) {
    EvidenceRecord record;
    std::uint8_t kind = 0;
    std::uint8_t durability = 0;
    std::uint8_t source_kind = 0;
    std::uint8_t enum_byte = 0;
    std::uint64_t source_id = 0;

    if (!reader.u8(kind) || !reader.u8(durability) || !reader.u64(record.generation.value) ||
        !reader.u64(source_id) || !reader.u8(source_kind) ||
        !reader.u64(record.provenance.sequence) || !reader.u64(record.provenance.observed_at_ms)) {
        return Status(StatusCode::kTruncated, "evidence record header is truncated");
    }
    record.kind = static_cast<EvidenceKind>(kind);
    record.durability = static_cast<Durability>(durability);
    record.provenance.kind = static_cast<SourceKind>(source_kind);
    record.provenance.source = SourceId(source_id);

    const EvidenceLimits& limits = default_evidence_limits();
    switch (record.kind) {
        case EvidenceKind::kMember: {
            MemberClaim claim;
            std::uint64_t rack = 0;
            std::uint64_t device = 0;
            if (!reader.u64(rack) || !reader.u64(device) || !reader.u64(claim.incarnation)) {
                return Status(StatusCode::kTruncated, "member claim is truncated");
            }
            claim.rack = RackId(rack);
            claim.device = DeviceId(device);
            record.payload = claim;
            break;
        }
        case EvidenceKind::kDevice: {
            DeviceClaim claim;
            std::uint64_t device = 0;
            std::uint8_t availability = 0;
            if (!reader.u64(device) || !reader.u8(enum_byte) || !reader.u64(claim.capacity.units) ||
                !reader.boolean(claim.capacity_known) || !reader.u8(availability)) {
                return Status(StatusCode::kTruncated, "device claim is truncated");
            }
            claim.device = DeviceId(device);
            claim.role = static_cast<DeviceRole>(enum_byte);
            claim.availability = static_cast<Availability>(availability);
            record.payload = claim;
            break;
        }
        case EvidenceKind::kPort: {
            PortClaim claim;
            std::uint64_t port = 0;
            std::uint64_t device = 0;
            std::uint8_t admin = 0;
            std::uint8_t availability = 0;
            if (!reader.u64(port) || !reader.u64(device) || !reader.u16(claim.index) ||
                !reader.u8(enum_byte) || !reader.u64(claim.capacity.units) ||
                !reader.boolean(claim.capacity_known) || !reader.u8(admin) ||
                !reader.u8(availability)) {
                return Status(StatusCode::kTruncated, "port claim is truncated");
            }
            claim.port = PortId(port);
            claim.device = DeviceId(device);
            claim.role = static_cast<PortRole>(enum_byte);
            claim.admin = static_cast<AdminState>(admin);
            claim.availability = static_cast<Availability>(availability);
            record.payload = claim;
            break;
        }
        case EvidenceKind::kLink: {
            LinkClaim claim;
            std::uint64_t link = 0;
            std::uint64_t port_a = 0;
            std::uint64_t port_b = 0;
            std::uint8_t availability = 0;
            if (!reader.u64(link) || !reader.u64(port_a) || !reader.u64(port_b) ||
                !reader.u8(enum_byte) || !reader.u64(claim.capacity.units) ||
                !reader.boolean(claim.capacity_known) || !reader.u8(availability)) {
                return Status(StatusCode::kTruncated, "link claim is truncated");
            }
            claim.link = LinkId(link);
            claim.port_a = PortId(port_a);
            claim.port_b = PortId(port_b);
            claim.kind = static_cast<LinkKind>(enum_byte);
            claim.availability = static_cast<Availability>(availability);
            record.payload = claim;
            break;
        }
        case EvidenceKind::kAttachment: {
            AttachmentClaim claim;
            std::uint64_t attachment = 0;
            std::uint64_t device = 0;
            std::uint64_t port = 0;
            if (!reader.u64(attachment) || !reader.u64(device) || !reader.u64(port)) {
                return Status(StatusCode::kTruncated, "attachment claim is truncated");
            }
            if (!reader.text(limits.max_host_key, claim.host_key)) {
                return Status(StatusCode::kMalformedEncoding,
                              "attachment host key is truncated or not UTF-8");
            }
            claim.attachment = AttachmentId(attachment);
            claim.device = DeviceId(device);
            claim.port = PortId(port);
            record.payload = claim;
            break;
        }
        case EvidenceKind::kMaintenance: {
            MaintenanceClaim claim;
            std::uint8_t action = 0;
            std::uint8_t target_kind = 0;
            if (!reader.u8(action) || !reader.u8(target_kind) || !reader.u64(claim.target_id)) {
                return Status(StatusCode::kTruncated, "maintenance claim is truncated");
            }
            if (!reader.text(limits.max_reason, claim.reason)) {
                return Status(StatusCode::kMalformedEncoding,
                              "maintenance reason is truncated or not UTF-8");
            }
            claim.action = static_cast<MaintenanceKind>(action);
            claim.target_kind = static_cast<ResourceKind>(target_kind);
            record.payload = claim;
            break;
        }
        case EvidenceKind::kObligation: {
            ObligationClaim claim;
            std::uint64_t obligation = 0;
            std::uint8_t target_kind = 0;
            std::uint64_t holder = 0;
            if (!reader.u64(obligation) || !reader.u8(target_kind) || !reader.u64(claim.target_id) ||
                !reader.u64(claim.capacity.units) || !reader.u64(holder)) {
                return Status(StatusCode::kTruncated, "obligation claim is truncated");
            }
            if (!reader.text(limits.max_authority, claim.authority)) {
                return Status(StatusCode::kMalformedEncoding,
                              "obligation authority is truncated or not UTF-8");
            }
            claim.obligation = ObligationId(obligation);
            claim.target_kind = static_cast<ResourceKind>(target_kind);
            claim.holder = PrincipalId(holder);
            record.payload = claim;
            break;
        }
        default:
            return Status(StatusCode::kMalformedEncoding, "unknown evidence kind");
    }

    const Status valid = validate_evidence(record);
    if (!valid.ok()) {
        return valid;
    }
    return record;
}

Status validate_evidence(const EvidenceRecord& record) {
    const EvidenceLimits& limits = default_evidence_limits();

    switch (record.kind) {
        case EvidenceKind::kMember: {
            const auto& claim = std::get<MemberClaim>(record.payload);
            if (!claim.rack.is_set() || !claim.device.is_set()) {
                return Status(StatusCode::kInvalidIdentity, "member claim needs rack and device");
            }
            break;
        }
        case EvidenceKind::kDevice: {
            const auto& claim = std::get<DeviceClaim>(record.payload);
            if (!claim.device.is_set()) {
                return Status(StatusCode::kInvalidIdentity, "device claim needs a device id");
            }
            break;
        }
        case EvidenceKind::kPort: {
            const auto& claim = std::get<PortClaim>(record.payload);
            if (!claim.port.is_set() || !claim.device.is_set()) {
                return Status(StatusCode::kInvalidIdentity, "port claim needs port and device");
            }
            if (claim.index > 65535U) {
                return Status(StatusCode::kOutOfRange, "port index is out of range");
            }
            break;
        }
        case EvidenceKind::kLink: {
            const auto& claim = std::get<LinkClaim>(record.payload);
            if (!claim.link.is_set() || !claim.port_a.is_set() || !claim.port_b.is_set()) {
                return Status(StatusCode::kInvalidIdentity, "link claim needs link and two ports");
            }
            if (claim.port_a == claim.port_b) {
                return Status(StatusCode::kInvalidArgument,
                              "link endpoints must be distinct ports");
            }
            break;
        }
        case EvidenceKind::kAttachment: {
            const auto& claim = std::get<AttachmentClaim>(record.payload);
            if (!claim.attachment.is_set() || !claim.device.is_set() || !claim.port.is_set()) {
                return Status(StatusCode::kInvalidIdentity,
                              "attachment claim needs attachment, device and port");
            }
            if (claim.host_key.size() > limits.max_host_key) {
                return Status(StatusCode::kOversizeField, "host key exceeds its bound");
            }
            if (!is_valid_utf8(ByteSpan(
                    reinterpret_cast<const std::uint8_t*>(claim.host_key.data()),
                    claim.host_key.size()))) {
                return Status(StatusCode::kInvalidUnicode, "host key is not well-formed UTF-8");
            }
            break;
        }
        case EvidenceKind::kMaintenance: {
            const auto& claim = std::get<MaintenanceClaim>(record.payload);
            if (claim.target_kind == ResourceKind::kRack) {
                if (claim.target_id != 0) {
                    return Status(StatusCode::kInvalidScope,
                                  "rack-wide maintenance must not carry a resource id");
                }
            } else if (claim.target_id == 0) {
                return Status(StatusCode::kInvalidScope,
                              "maintenance target must name a rack or a resource");
            }
            if (claim.reason.size() > limits.max_reason) {
                return Status(StatusCode::kOversizeField, "maintenance reason exceeds its bound");
            }
            if (!is_valid_utf8(ByteSpan(reinterpret_cast<const std::uint8_t*>(claim.reason.data()),
                                        claim.reason.size()))) {
                return Status(StatusCode::kInvalidUnicode,
                              "maintenance reason is not well-formed UTF-8");
            }
            break;
        }
        case EvidenceKind::kObligation: {
            const auto& claim = std::get<ObligationClaim>(record.payload);
            if (!claim.obligation.is_set()) {
                return Status(StatusCode::kInvalidIdentity, "obligation claim needs an id");
            }
            if (!claim.holder.is_set()) {
                return Status(StatusCode::kInvalidIdentity, "obligation claim needs a holder");
            }
            if (claim.target_id == 0 && claim.target_kind != ResourceKind::kRack) {
                return Status(StatusCode::kInvalidScope, "obligation target must name a resource");
            }
            if (claim.authority.size() > limits.max_authority) {
                return Status(StatusCode::kOversizeField, "obligation authority exceeds its bound");
            }
            if (!is_valid_utf8(ByteSpan(
                    reinterpret_cast<const std::uint8_t*>(claim.authority.data()),
                    claim.authority.size()))) {
                return Status(StatusCode::kInvalidUnicode,
                              "obligation authority is not well-formed UTF-8");
            }
            break;
        }
    }

    switch (record.durability) {
        case Durability::kEphemeral:
        case Durability::kSticky:
            break;
        default:
            return Status(StatusCode::kOutOfRange, "unknown durability");
    }
    switch (record.provenance.kind) {
        case SourceKind::kOperator:
        case SourceKind::kDiscovery:
        case SourceKind::kImported:
        case SourceKind::kSynthetic:
            break;
        default:
            return Status(StatusCode::kOutOfRange, "unknown source kind");
    }
    if (!record.provenance.source.is_set()) {
        return Status(StatusCode::kInvalidIdentity, "evidence needs a source identity");
    }

    ByteWriter writer(kLimits.max_encoded_record);
    RNF_TRYV(encode(record, writer));
    return Status{};
}

Digest evidence_digest(const EvidenceRecord& record) {
    ByteWriter writer(kLimits.max_encoded_record);
    const Status status = encode(record, writer);
    if (!status.ok()) {
        // A record that cannot be encoded is still given a stable digest so the
        // ledger can order it deterministically; validation is what refuses it.
        Blake2s256 fallback;
        fallback.update("rnf.unencodable");
        fallback.update_le64(static_cast<std::uint64_t>(status.code()));
        return fallback.final();
    }
    return Blake2s256::hash(writer.span());
}

std::string describe(const EvidenceRecord& record) {
    std::string out(to_string(record.kind));
    out += " gen=";
    out += std::to_string(record.generation.value);
    out += " src=";
    out += record.provenance.source.to_hex();
    out += " seq=";
    out += std::to_string(record.provenance.sequence);
    out += " ";
    switch (record.kind) {
        case EvidenceKind::kMember: {
            const auto& claim = std::get<MemberClaim>(record.payload);
            out += "rack=" + claim.rack.to_hex() + " device=" + claim.device.to_hex() +
                   " incarnation=" + std::to_string(claim.incarnation);
            break;
        }
        case EvidenceKind::kDevice: {
            const auto& claim = std::get<DeviceClaim>(record.payload);
            out += "device=" + claim.device.to_hex() + " role=" +
                   std::string(to_string(claim.role)) + " capacity=" +
                   (claim.capacity_known ? std::to_string(claim.capacity.units)
                                         : std::string("unknown")) +
                   " availability=" + std::string(to_string(claim.availability));
            break;
        }
        case EvidenceKind::kPort: {
            const auto& claim = std::get<PortClaim>(record.payload);
            out += "port=" + claim.port.to_hex() + " device=" + claim.device.to_hex() + " index=" +
                   std::to_string(claim.index) + " capacity=" +
                   (claim.capacity_known ? std::to_string(claim.capacity.units)
                                         : std::string("unknown")) +
                   " admin=" +
                   std::string(to_string(claim.admin)) + " availability=" +
                   std::string(to_string(claim.availability));
            break;
        }
        case EvidenceKind::kLink: {
            const auto& claim = std::get<LinkClaim>(record.payload);
            out += "link=" + claim.link.to_hex() + " a=" + claim.port_a.to_hex() +
                   " b=" + claim.port_b.to_hex() + " kind=" +
                   std::string(to_string(claim.kind)) + " capacity=" +
                   (claim.capacity_known ? std::to_string(claim.capacity.units)
                                         : std::string("unknown")) +
                   " availability=" +
                   std::string(to_string(claim.availability));
            break;
        }
        case EvidenceKind::kAttachment: {
            const auto& claim = std::get<AttachmentClaim>(record.payload);
            out += "attachment=" + claim.attachment.to_hex() + " device=" +
                   claim.device.to_hex() + " port=" + claim.port.to_hex() + " host=" +
                   claim.host_key;
            break;
        }
        case EvidenceKind::kMaintenance: {
            const auto& claim = std::get<MaintenanceClaim>(record.payload);
            out += "action=" + std::string(to_string(claim.action)) + " target=" +
                   ResourceRef{claim.target_kind, claim.target_id}.to_text() + " reason=" +
                   claim.reason;
            break;
        }
        case EvidenceKind::kObligation: {
            const auto& claim = std::get<ObligationClaim>(record.payload);
            out += "obligation=" + claim.obligation.to_hex() + " target=" +
                   ResourceRef{claim.target_kind, claim.target_id}.to_text() + " capacity=" +
                   std::to_string(claim.capacity.units) + " holder=" + claim.holder.to_hex() +
                   " authority=" + claim.authority;
            break;
        }
    }
    return out;
}

std::string_view to_string(InsertOutcome outcome) noexcept {
    switch (outcome) {
        case InsertOutcome::kInserted: return "inserted";
        case InsertOutcome::kUpdated: return "updated";
        case InsertOutcome::kDuplicate: return "duplicate";
        case InsertOutcome::kSuperseded: return "superseded";
        case InsertOutcome::kConflicted: return "conflicted";
        case InsertOutcome::kRefused: return "refused";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// EvidenceLedger
// ---------------------------------------------------------------------------

InsertReport EvidenceLedger::insert(const EvidenceRecord& record) {
    InsertReport report;
    report.slot = slot_of(record);
    report.record_digest = evidence_digest(record);

    const Status valid = validate_evidence(record);
    if (!valid.ok()) {
        report.outcome = InsertOutcome::kRefused;
        report.status = valid;
        return report;
    }

    const auto existing = slots_.find(report.slot);
    if (existing == slots_.end()) {
        if (slots_.size() >= limits_.max_slots) {
            report.outcome = InsertOutcome::kRefused;
            report.status = Status(StatusCode::kResourceExhausted,
                                   "evidence ledger reached its slot bound");
            return report;
        }
        if (!sources_.count(record.provenance.source) &&
            sources_.size() >= limits_.max_sources) {
            report.outcome = InsertOutcome::kRefused;
            report.status = Status(StatusCode::kResourceExhausted,
                                   "evidence ledger reached its source bound");
            return report;
        }
        slots_.emplace(report.slot, record);
        digests_.emplace(report.slot, report.record_digest);
        sources_.insert(record.provenance.source);
        report.outcome = InsertOutcome::kInserted;
        report.mutated = true;
        return report;
    }

    const Digest stored_digest = digests_.at(report.slot);
    if (stored_digest == report.record_digest) {
        report.outcome = InsertOutcome::kDuplicate;
        return report;
    }

    const auto incoming_rank = rank_of(record);
    const auto stored_rank = rank_of(existing->second);
    SlotHistory& history = history_[report.slot];

    if (incoming_rank < stored_rank) {
        ++history.superseded_count;
        ++superseded_;
        if (history.superseded_min.is_zero() || report.record_digest < history.superseded_min) {
            history.superseded_min = report.record_digest;
        }
        report.outcome = InsertOutcome::kSuperseded;
        report.status = Status(StatusCode::kSuperseded,
                               "an older observation for this slot is already stored");
        return report;
    }

    if (incoming_rank > stored_rank) {
        // The stored record is replaced here, not ignored. Only an observation
        // that loses to an already stored newer one counts as superseded.
        slots_[report.slot] = record;
        digests_[report.slot] = report.record_digest;
        report.outcome = InsertOutcome::kUpdated;
        report.mutated = true;
        return report;
    }

    // Same rank, different content: two observations of the same subject from
    // the same source at the same generation. Keep the lexicographically
    // smaller digest so the outcome does not depend on arrival order.
    ++history.conflict_count;
    ++conflicts_;
    if (history.conflicting_min.is_zero() || report.record_digest < history.conflicting_min) {
        history.conflicting_min = report.record_digest;
    }
    report.outcome = InsertOutcome::kConflicted;
    report.status = Status(StatusCode::kConflict,
                           "two observations share a generation and sequence but disagree");
    if (report.record_digest < stored_digest) {
        slots_[report.slot] = record;
        digests_[report.slot] = report.record_digest;
        report.mutated = true;
    }
    return report;
}

bool EvidenceLedger::contains(const EvidenceSlot& slot) const {
    return slots_.find(slot) != slots_.end();
}

const EvidenceRecord* EvidenceLedger::find(const EvidenceSlot& slot) const {
    const auto it = slots_.find(slot);
    return it == slots_.end() ? nullptr : &it->second;
}

Digest EvidenceLedger::digest() const {
    Blake2s256 hasher;
    hasher.update("rnf.ledger.v1");
    for (const auto& [slot, record] : slots_) {
        (void)slot;
        const Digest record_hash = evidence_digest(record);
        hasher.update(ByteSpan(record_hash.bytes.data(), record_hash.bytes.size()));
    }
    return hasher.final();
}

std::vector<EvidenceRecord> EvidenceLedger::records() const {
    std::vector<EvidenceRecord> out;
    out.reserve(slots_.size());
    for (const auto& entry : slots_) {
        out.push_back(entry.second);
    }
    return out;
}

void EvidenceLedger::clear() {
    slots_.clear();
    digests_.clear();
    history_.clear();
    sources_.clear();
    conflicts_ = 0;
    superseded_ = 0;
}

}  // namespace rnf
