// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Rack-scoped domain vocabulary: generations, incarnations, capacity, roles,
// availability, maintenance, lifecycle and the typed diagnostics produced when
// evidence is stale, missing or contradictory.

#ifndef RNF_MODEL_TYPES_HPP
#define RNF_MODEL_TYPES_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rnf/core/bytes.hpp"
#include "rnf/core/checked.hpp"
#include "rnf/core/id.hpp"

namespace rnf {

// ---------------------------------------------------------------------------
// Generations and incarnations
// ---------------------------------------------------------------------------

/// Monotonic composition generation of the rack's accepted evidence.
struct TopologyGeneration {
    std::uint64_t value = 0;
    friend constexpr bool operator==(const TopologyGeneration&, const TopologyGeneration&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const TopologyGeneration& a,
                                                      const TopologyGeneration& b) noexcept {
        return a.value <=> b.value;
    }
};

/// Monotonic rack membership epoch. Advances exactly when the set of member
/// devices changes. Authority bound to an older epoch is fenced.
struct RackEpoch {
    std::uint64_t value = 0;
    friend constexpr bool operator==(const RackEpoch&, const RackEpoch&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const RackEpoch& a, const RackEpoch& b) noexcept {
        return a.value <=> b.value;
    }
};

/// Monotonic daemon-process incarnation, persisted and bumped on every start.
/// Because it strictly increases across restarts it fences authority issued by
/// a previous process even when the previous process died without warning.
struct ControllerIncarnation {
    std::uint64_t value = 0;
    friend constexpr bool operator==(const ControllerIncarnation&,
                                     const ControllerIncarnation&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const ControllerIncarnation& a,
                                                      const ControllerIncarnation& b) noexcept {
        return a.value <=> b.value;
    }
};

/// Per-device membership incarnation. Changes whenever a device leaves and
/// rejoins, which invalidates authority that depended on the previous member.
using MemberIncarnation = std::uint64_t;

/// Monotonic fencing token, unique per issued lease within a rack.
using FencingToken = std::uint64_t;

/// Milliseconds since the Unix epoch. Only ever compared, never assumed
/// monotonic across processes.
using TimestampMs = std::uint64_t;

/// Per-source monotonically increasing evidence sequence number.
using Sequence = std::uint64_t;

// ---------------------------------------------------------------------------
// Capacity
// ---------------------------------------------------------------------------

/// Abstract capacity units. The runtime never interprets the unit; the operator
/// decides whether it means kilobits per second, queue slots or anything else,
/// and every value in one rack must use the same unit. All arithmetic is
/// checked, so a capacity computation either closes exactly or fails loudly.
struct Capacity {
    std::uint64_t units = 0;

    friend constexpr bool operator==(const Capacity&, const Capacity&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const Capacity& a, const Capacity& b) noexcept {
        return a.units <=> b.units;
    }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return units == 0; }
};

[[nodiscard]] inline Result<Capacity> capacity_add(Capacity a, Capacity b) {
    std::uint64_t out = 0;
    if (!checked_add_u64(a.units, b.units, out)) {
        return Status(StatusCode::kOutOfRange, "capacity addition overflowed");
    }
    return Capacity{out};
}

[[nodiscard]] inline Result<Capacity> capacity_sub(Capacity a, Capacity b) {
    std::uint64_t out = 0;
    if (!checked_sub_u64(a.units, b.units, out)) {
        return Status(StatusCode::kCapacityExhausted, "capacity subtraction underflowed");
    }
    return Capacity{out};
}

// ---------------------------------------------------------------------------
// Roles and states
// ---------------------------------------------------------------------------

enum class DeviceRole : std::uint8_t {
    kUnknown = 0,
    kSwitch = 1,
    kHost = 2,
    kAppliance = 3,
    kManagement = 4,
    kSynthetic = 5,
};

enum class PortRole : std::uint8_t {
    kUnknown = 0,
    kAccess = 1,
    kUplink = 2,
    kFabric = 3,
    kPeer = 4,
    kManagement = 5,
};

enum class LinkKind : std::uint8_t {
    kUnknown = 0,
    kPhysical = 1,
    kLogical = 2,
    kPeer = 3,
};

enum class AdminState : std::uint8_t {
    kUnknown = 0,
    kEnabled = 1,
    kDisabled = 2,
    kDraining = 3,
};

/// Operational availability. kUnknown is never promoted to kUp.
enum class Availability : std::uint8_t {
    kUnknown = 0,
    kDown = 1,
    kMaintenance = 2,
    kDegraded = 3,
    kUp = 4,
};

/// Usability rank; higher is more usable. Unknown ranks lowest on purpose.
[[nodiscard]] constexpr unsigned availability_rank(Availability a) noexcept {
    return static_cast<unsigned>(a);
}

/// Combine two availability observations, keeping the least usable one.
[[nodiscard]] constexpr Availability combine_availability(Availability a, Availability b) noexcept {
    return availability_rank(a) <= availability_rank(b) ? a : b;
}

/// A resource can be used for new authority only when this is kEligible.
enum class Eligibility : std::uint8_t {
    kEligible = 1,
    kIneligible = 2,
    kUnknown = 3,
    kIncomplete = 4,
};

enum class ResourceKind : std::uint8_t {
    kRack = 1,
    kDevice = 2,
    kPort = 3,
    kLink = 4,
    kAttachment = 5,
    kPath = 6,
};

/// A concrete resource the authority layer can name in a scope.
struct ResourceRef {
    ResourceKind kind = ResourceKind::kRack;
    std::uint64_t id = 0;

    friend constexpr bool operator==(const ResourceRef&, const ResourceRef&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const ResourceRef& a,
                                                      const ResourceRef& b) noexcept {
        if (a.kind != b.kind) {
            return a.kind <=> b.kind;
        }
        return a.id <=> b.id;
    }
    [[nodiscard]] std::string to_text() const;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/// Rack lifecycle.
///
///   assembling -> active -> degraded -> draining -> maintenance
///                        \-> recovering -> active
///   any -> retired (terminal)
enum class LifecycleState : std::uint8_t {
    kAssembling = 1,
    kActive = 2,
    kDegraded = 3,
    kDraining = 4,
    kMaintenance = 5,
    kRecovering = 6,
    kRetired = 7,
};

/// True when the lifecycle permits issuing new authority.
[[nodiscard]] constexpr bool lifecycle_accepts_new_authority(LifecycleState state) noexcept {
    return state == LifecycleState::kActive || state == LifecycleState::kDegraded;
}

/// True when the lifecycle permits validation of already issued authority.
[[nodiscard]] constexpr bool lifecycle_honours_existing_authority(LifecycleState state) noexcept {
    switch (state) {
        case LifecycleState::kActive:
        case LifecycleState::kDegraded:
        case LifecycleState::kDraining:
            return true;
        case LifecycleState::kAssembling:
        case LifecycleState::kMaintenance:
        case LifecycleState::kRecovering:
        case LifecycleState::kRetired:
            return false;
    }
    return false;
}

/// Legal lifecycle transitions. Refusals are typed, never silent.
[[nodiscard]] bool lifecycle_transition_allowed(LifecycleState from, LifecycleState to) noexcept;

// ---------------------------------------------------------------------------
// Evidence vocabulary
// ---------------------------------------------------------------------------

enum class EvidenceKind : std::uint8_t {
    kMember = 1,
    kDevice = 2,
    kPort = 3,
    kLink = 4,
    kAttachment = 5,
    kMaintenance = 6,
    kObligation = 7,
};

enum class SourceKind : std::uint8_t {
    kOperator = 1,
    kDiscovery = 2,
    kImported = 3,
    kSynthetic = 4,
};

/// How long an observation stays applicable.
///
/// kEphemeral observations apply only at the exact generation they name: a
/// discovery snapshot from generation 7 says nothing about generation 8.
/// kSticky observations carry forward until superseded by a newer observation
/// from the same source, which is what makes maintenance and imported
/// obligations survive both a generation bump and a replayed older message.
enum class Durability : std::uint8_t {
    kEphemeral = 1,
    kSticky = 2,
};

enum class MaintenanceKind : std::uint8_t {
    kDrain = 1,
    kMaintenance = 2,
    kClear = 3,
};

/// Why a composed resource is not usable. Distinct from "not present".
enum class DiagnosticKind : std::uint8_t {
    kStaleEvidence = 1,
    kSupersededEvidence = 2,
    kConflictingEvidence = 3,
    kIncompleteTopology = 4,
    kOutOfRack = 5,
    kUnknownCapacity = 6,
    kCapacityOvercommit = 7,
    kMaintenanceExclusion = 8,
    kUnsupportedObservation = 9,
    kUnknownMember = 10,
    kReplayedEvidence = 11,
    kUnresolvedReference = 12,
};

struct Diagnostic {
    DiagnosticKind kind = DiagnosticKind::kStaleEvidence;
    EvidenceKind subject_kind = EvidenceKind::kMember;
    std::uint64_t subject_id = 0;
    SourceId source{};
    std::string detail;

    friend bool operator==(const Diagnostic&, const Diagnostic&) = default;
    friend std::strong_ordering operator<=>(const Diagnostic& a, const Diagnostic& b) noexcept {
        if (a.kind != b.kind) {
            return a.kind <=> b.kind;
        }
        if (a.subject_kind != b.subject_kind) {
            return a.subject_kind <=> b.subject_kind;
        }
        if (a.subject_id != b.subject_id) {
            return a.subject_id <=> b.subject_id;
        }
        if (a.source != b.source) {
            return a.source <=> b.source;
        }
        return a.detail <=> b.detail;
    }
};

// ---------------------------------------------------------------------------
// Text conversion
// ---------------------------------------------------------------------------

[[nodiscard]] std::string_view to_string(DeviceRole value) noexcept;
[[nodiscard]] std::string_view to_string(PortRole value) noexcept;
[[nodiscard]] std::string_view to_string(LinkKind value) noexcept;
[[nodiscard]] std::string_view to_string(AdminState value) noexcept;
[[nodiscard]] std::string_view to_string(Availability value) noexcept;
[[nodiscard]] std::string_view to_string(Eligibility value) noexcept;
[[nodiscard]] std::string_view to_string(ResourceKind value) noexcept;
[[nodiscard]] std::string_view to_string(LifecycleState value) noexcept;
[[nodiscard]] std::string_view to_string(EvidenceKind value) noexcept;
[[nodiscard]] std::string_view to_string(SourceKind value) noexcept;
[[nodiscard]] std::string_view to_string(Durability value) noexcept;
[[nodiscard]] std::string_view to_string(MaintenanceKind value) noexcept;
[[nodiscard]] std::string_view to_string(DiagnosticKind value) noexcept;

[[nodiscard]] bool parse_device_role(std::string_view text, DeviceRole& out) noexcept;
[[nodiscard]] bool parse_port_role(std::string_view text, PortRole& out) noexcept;
[[nodiscard]] bool parse_link_kind(std::string_view text, LinkKind& out) noexcept;
[[nodiscard]] bool parse_admin_state(std::string_view text, AdminState& out) noexcept;
[[nodiscard]] bool parse_availability(std::string_view text, Availability& out) noexcept;
[[nodiscard]] bool parse_lifecycle_state(std::string_view text, LifecycleState& out) noexcept;
[[nodiscard]] bool parse_resource_kind(std::string_view text, ResourceKind& out) noexcept;
[[nodiscard]] bool parse_maintenance_kind(std::string_view text, MaintenanceKind& out) noexcept;
[[nodiscard]] bool parse_source_kind(std::string_view text, SourceKind& out) noexcept;
[[nodiscard]] bool parse_durability(std::string_view text, Durability& out) noexcept;

/// Parse "<kind>:<id>", for example "device:7" or "port:12".
[[nodiscard]] Result<ResourceRef> parse_resource_ref(std::string_view text);

}  // namespace rnf

#endif  // RNF_MODEL_TYPES_HPP
