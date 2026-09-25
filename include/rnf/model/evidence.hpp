// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Evidence: the only way information enters the runtime. Every record carries
// its source, that source's own sequence number, the topology generation it was
// observed against, and how long it stays applicable. Nothing is inferred from
// the arrival order of records.

#ifndef RNF_MODEL_EVIDENCE_HPP
#define RNF_MODEL_EVIDENCE_HPP

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "rnf/core/bytes.hpp"
#include "rnf/model/types.hpp"

namespace rnf {

/// Hard bounds. Exceeding any of them is a typed refusal, never a truncation.
struct EvidenceLimits {
    std::size_t max_slots = 1U << 20;          // distinct (kind, subject, source)
    std::size_t max_sources = 4096;            // distinct source identities
    std::size_t max_records_per_batch = 4096;  // one SubmitEvidence message
    std::size_t max_host_key = 128;
    std::size_t max_reason = 256;
    std::size_t max_authority = 128;
    std::size_t max_encoded_record = 512;
};

[[nodiscard]] const EvidenceLimits& default_evidence_limits() noexcept;

/// Where a record came from and how fresh the source believes it is.
struct Provenance {
    SourceId source{};
    SourceKind kind = SourceKind::kOperator;
    Sequence sequence = 0;
    TimestampMs observed_at_ms = 0;

    friend bool operator==(const Provenance&, const Provenance&) = default;
    friend std::strong_ordering operator<=>(const Provenance& a, const Provenance& b) noexcept {
        if (a.source != b.source) {
            return a.source <=> b.source;
        }
        if (a.sequence != b.sequence) {
            return a.sequence <=> b.sequence;
        }
        if (a.kind != b.kind) {
            return a.kind <=> b.kind;
        }
        return a.observed_at_ms <=> b.observed_at_ms;
    }
};

/// The rack claims a device as a member, under a specific member incarnation.
struct MemberClaim {
    RackId rack{};
    DeviceId device{};
    MemberIncarnation incarnation = 0;
    friend bool operator==(const MemberClaim&, const MemberClaim&) = default;
};

struct DeviceClaim {
    DeviceId device{};
    DeviceRole role = DeviceRole::kUnknown;
    Capacity capacity{};
    /// False when the source could not measure capacity. An unknown capacity is
    /// never treated as zero and never treated as sufficient.
    bool capacity_known = true;
    /// Device level availability. There is no default: a device that nobody has
    /// reported on is kUnknown and its capacity counts as unavailable.
    Availability availability = Availability::kUnknown;
    friend bool operator==(const DeviceClaim&, const DeviceClaim&) = default;
};

struct PortClaim {
    PortId port{};
    DeviceId device{};
    std::uint16_t index = 0;
    PortRole role = PortRole::kUnknown;
    Capacity capacity{};
    bool capacity_known = true;
    AdminState admin = AdminState::kUnknown;
    Availability availability = Availability::kUnknown;
    friend bool operator==(const PortClaim&, const PortClaim&) = default;
};

struct LinkClaim {
    LinkId link{};
    PortId port_a{};
    PortId port_b{};
    LinkKind kind = LinkKind::kUnknown;
    Capacity capacity{};
    bool capacity_known = true;
    Availability availability = Availability::kUnknown;
    friend bool operator==(const LinkClaim&, const LinkClaim&) = default;
};

struct AttachmentClaim {
    AttachmentId attachment{};
    DeviceId device{};
    PortId port{};
    std::string host_key;
    friend bool operator==(const AttachmentClaim&, const AttachmentClaim&) = default;
};

struct MaintenanceClaim {
    MaintenanceKind action = MaintenanceKind::kDrain;
    ResourceKind target_kind = ResourceKind::kDevice;
    std::uint64_t target_id = 0;
    std::string reason;
    friend bool operator==(const MaintenanceClaim&, const MaintenanceClaim&) = default;
};

/// A reservation imposed on rack resources by governance outside the rack.
/// The obligation constrains in-rack capacity; it does not grant it.
struct ObligationClaim {
    ObligationId obligation{};
    ResourceKind target_kind = ResourceKind::kDevice;
    std::uint64_t target_id = 0;
    Capacity capacity{};
    PrincipalId holder{};
    std::string authority;
    friend bool operator==(const ObligationClaim&, const ObligationClaim&) = default;
};

using EvidencePayload =
    std::variant<MemberClaim, DeviceClaim, PortClaim, LinkClaim, AttachmentClaim, MaintenanceClaim,
                 ObligationClaim>;

/// One accepted unit of evidence.
struct EvidenceRecord {
    EvidenceKind kind = EvidenceKind::kMember;
    Durability durability = Durability::kSticky;
    TopologyGeneration generation{};
    Provenance provenance{};
    EvidencePayload payload{};

    friend bool operator==(const EvidenceRecord&, const EvidenceRecord&) = default;
};

/// Identity of the slot a record occupies. Two records in the same slot are
/// competing claims about the same subject from the same source.
struct EvidenceSlot {
    EvidenceKind kind = EvidenceKind::kMember;
    std::uint64_t subject_a = 0;
    std::uint64_t subject_b = 0;
    SourceId source{};

    friend constexpr bool operator==(const EvidenceSlot&, const EvidenceSlot&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const EvidenceSlot& a,
                                                      const EvidenceSlot& b) noexcept {
        if (a.kind != b.kind) {
            return a.kind <=> b.kind;
        }
        if (a.subject_a != b.subject_a) {
            return a.subject_a <=> b.subject_a;
        }
        if (a.subject_b != b.subject_b) {
            return a.subject_b <=> b.subject_b;
        }
        return a.source <=> b.source;
    }
    [[nodiscard]] std::string to_text() const;
};

/// Slot computation. Deterministic and independent of arrival order.
[[nodiscard]] EvidenceSlot slot_of(const EvidenceRecord& record) noexcept;

/// Canonical encoding of a record.
[[nodiscard]] Status encode(const EvidenceRecord& record, ByteWriter& writer);
[[nodiscard]] Result<EvidenceRecord> decode_evidence(ByteReader& reader);

/// Content address of a record. Used for de-duplication and for the ledger
/// digest, which is what makes "equivalent evidence => equivalent state"
/// checkable.
[[nodiscard]] Digest evidence_digest(const EvidenceRecord& record);

/// Structural validation: identity presence, field bounds, UTF-8, self
/// consistency. Does not consult any rack configuration.
[[nodiscard]] Status validate_evidence(const EvidenceRecord& record);

/// Human readable one-line rendering, used by the CLI and by diagnostics.
[[nodiscard]] std::string describe(const EvidenceRecord& record);

/// Result of offering a record to the ledger.
enum class InsertOutcome : std::uint8_t {
    kInserted = 1,     // new slot
    kUpdated = 2,      // same slot, newer rank replaced the stored record
    kDuplicate = 3,    // byte-identical record already stored
    kSuperseded = 4,   // stored record dominates; incoming ignored
    kConflicted = 5,   // same rank, different content; winner chosen deterministically
    kRefused = 6,      // malformed or over a configured bound
};

[[nodiscard]] std::string_view to_string(InsertOutcome outcome) noexcept;

/// Summary of one insert attempt.
struct InsertReport {
    InsertOutcome outcome = InsertOutcome::kInserted;
    Status status{};
    EvidenceSlot slot{};
    Digest record_digest{};
    /// True when the stored state actually changed.
    bool mutated = false;
};

/// Order-independent aggregate of the losing records seen for one slot.
///
/// Every field is either a count or a minimum, so the value depends only on the
/// multiset of records offered, never on the order in which they arrived. This
/// is what lets the composer emit identical diagnostics for equivalent
/// evidence regardless of arrival order.
struct SlotHistory {
    std::uint64_t superseded_count = 0;
    std::uint64_t conflict_count = 0;
    Digest superseded_min{};
    Digest conflicting_min{};
};

/// The accepted-evidence set.
///
/// Threading: not internally synchronised. Callers serialise access; rnfd
/// guards the ledger with the daemon state mutex.
///
/// Ordering: the ledger is keyed by EvidenceSlot, so iteration order is a pure
/// function of the slot keys and not of insertion order. For each slot the
/// stored record is the one with the highest (topology generation, source
/// sequence); ties with identical content are idempotent, ties with differing
/// content are resolved by the lexicographically smaller content digest and
/// reported as kConflicted. Both rules together make the ledger arrival-order
/// independent for equivalent accepted evidence.
class EvidenceLedger {
public:
    explicit EvidenceLedger(EvidenceLimits limits = default_evidence_limits()) : limits_(limits) {}

    [[nodiscard]] InsertReport insert(const EvidenceRecord& record);

    [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }
    [[nodiscard]] const EvidenceLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] const std::map<EvidenceSlot, EvidenceRecord>& slots() const noexcept {
        return slots_;
    }
    [[nodiscard]] bool contains(const EvidenceSlot& slot) const;
    [[nodiscard]] const EvidenceRecord* find(const EvidenceSlot& slot) const;
    [[nodiscard]] std::size_t source_count() const noexcept { return sources_.size(); }

    /// Canonical digest over every stored record, independent of insert order.
    [[nodiscard]] Digest digest() const;

    /// Records in canonical slot order.
    [[nodiscard]] std::vector<EvidenceRecord> records() const;

    /// Total conflicting observations observed since construction.
    [[nodiscard]] std::uint64_t conflict_count() const noexcept { return conflicts_; }
    /// Total superseded observations observed since construction.
    [[nodiscard]] std::uint64_t superseded_count() const noexcept { return superseded_; }

    [[nodiscard]] const std::map<EvidenceSlot, SlotHistory>& slot_history() const noexcept {
        return history_;
    }

    void clear();

private:
    EvidenceLimits limits_;
    std::map<EvidenceSlot, EvidenceRecord> slots_;
    std::map<EvidenceSlot, Digest> digests_;
    std::map<EvidenceSlot, SlotHistory> history_;
    std::set<SourceId> sources_;
    std::uint64_t conflicts_ = 0;
    std::uint64_t superseded_ = 0;
};

}  // namespace rnf

#endif  // RNF_MODEL_EVIDENCE_HPP
