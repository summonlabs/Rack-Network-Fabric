// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/model/types.hpp"

namespace rnf {
namespace {

template <class Enum>
bool parse_enum(std::string_view text, Enum& out, const std::pair<std::string_view, Enum>* table,
                std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        if (table[i].first == text) {
            out = table[i].second;
            return true;
        }
    }
    return false;
}

constexpr std::pair<std::string_view, DeviceRole> kDeviceRoles[] = {
    {"unknown", DeviceRole::kUnknown},   {"switch", DeviceRole::kSwitch},
    {"host", DeviceRole::kHost},         {"appliance", DeviceRole::kAppliance},
    {"management", DeviceRole::kManagement}, {"synthetic", DeviceRole::kSynthetic},
};

constexpr std::pair<std::string_view, PortRole> kPortRoles[] = {
    {"unknown", PortRole::kUnknown}, {"access", PortRole::kAccess}, {"uplink", PortRole::kUplink},
    {"fabric", PortRole::kFabric},   {"peer", PortRole::kPeer},
    {"management", PortRole::kManagement},
};

constexpr std::pair<std::string_view, LinkKind> kLinkKinds[] = {
    {"unknown", LinkKind::kUnknown},
    {"physical", LinkKind::kPhysical},
    {"logical", LinkKind::kLogical},
    {"peer", LinkKind::kPeer},
};

constexpr std::pair<std::string_view, AdminState> kAdminStates[] = {
    {"unknown", AdminState::kUnknown},
    {"enabled", AdminState::kEnabled},
    {"disabled", AdminState::kDisabled},
    {"draining", AdminState::kDraining},
};

constexpr std::pair<std::string_view, Availability> kAvailabilities[] = {
    {"unknown", Availability::kUnknown}, {"down", Availability::kDown},
    {"maintenance", Availability::kMaintenance}, {"degraded", Availability::kDegraded},
    {"up", Availability::kUp},
};

constexpr std::pair<std::string_view, LifecycleState> kLifecycleStates[] = {
    {"assembling", LifecycleState::kAssembling}, {"active", LifecycleState::kActive},
    {"degraded", LifecycleState::kDegraded},     {"draining", LifecycleState::kDraining},
    {"maintenance", LifecycleState::kMaintenance}, {"recovering", LifecycleState::kRecovering},
    {"retired", LifecycleState::kRetired},
};

constexpr std::pair<std::string_view, ResourceKind> kResourceKinds[] = {
    {"rack", ResourceKind::kRack},       {"device", ResourceKind::kDevice},
    {"port", ResourceKind::kPort},       {"link", ResourceKind::kLink},
    {"attachment", ResourceKind::kAttachment}, {"path", ResourceKind::kPath},
};

constexpr std::pair<std::string_view, MaintenanceKind> kMaintenanceKinds[] = {
    {"drain", MaintenanceKind::kDrain},
    {"maintenance", MaintenanceKind::kMaintenance},
    {"clear", MaintenanceKind::kClear},
};

constexpr std::pair<std::string_view, SourceKind> kSourceKinds[] = {
    {"operator", SourceKind::kOperator}, {"discovery", SourceKind::kDiscovery},
    {"imported", SourceKind::kImported}, {"synthetic", SourceKind::kSynthetic},
};

constexpr std::pair<std::string_view, Durability> kDurabilities[] = {
    {"ephemeral", Durability::kEphemeral},
    {"sticky", Durability::kSticky},
};

template <class Table>
constexpr std::size_t table_size(const Table& table) noexcept {
    return sizeof(table) / sizeof(table[0]);
}

std::uint64_t parse_u64(std::string_view text, bool& ok) noexcept {
    ok = false;
    if (text.empty() || text.size() > 20) {
        return 0;
    }
    std::uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return 0;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return 0;
        }
        value = value * 10U + digit;
    }
    ok = true;
    return value;
}

}  // namespace

std::string ResourceRef::to_text() const {
    std::string out(to_string(kind));
    out.push_back(':');
    out += std::to_string(id);
    return out;
}

std::string_view to_string(DeviceRole value) noexcept {
    for (const auto& entry : kDeviceRoles) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return "unknown";
}

std::string_view to_string(PortRole value) noexcept {
    for (const auto& entry : kPortRoles) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return "unknown";
}

std::string_view to_string(LinkKind value) noexcept {
    for (const auto& entry : kLinkKinds) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return "unknown";
}

std::string_view to_string(AdminState value) noexcept {
    for (const auto& entry : kAdminStates) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return "unknown";
}

std::string_view to_string(Availability value) noexcept {
    for (const auto& entry : kAvailabilities) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return "unknown";
}

std::string_view to_string(Eligibility value) noexcept {
    switch (value) {
        case Eligibility::kEligible: return "eligible";
        case Eligibility::kIneligible: return "ineligible";
        case Eligibility::kUnknown: return "unknown";
        case Eligibility::kIncomplete: return "incomplete";
    }
    return "unknown";
}

std::string_view to_string(ResourceKind value) noexcept {
    for (const auto& entry : kResourceKinds) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return "unknown";
}

std::string_view to_string(LifecycleState value) noexcept {
    for (const auto& entry : kLifecycleStates) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return "unknown";
}

std::string_view to_string(EvidenceKind value) noexcept {
    switch (value) {
        case EvidenceKind::kMember: return "member";
        case EvidenceKind::kDevice: return "device";
        case EvidenceKind::kPort: return "port";
        case EvidenceKind::kLink: return "link";
        case EvidenceKind::kAttachment: return "attachment";
        case EvidenceKind::kMaintenance: return "maintenance";
        case EvidenceKind::kObligation: return "obligation";
    }
    return "unknown";
}

std::string_view to_string(SourceKind value) noexcept {
    for (const auto& entry : kSourceKinds) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return "unknown";
}

std::string_view to_string(Durability value) noexcept {
    for (const auto& entry : kDurabilities) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return "unknown";
}

std::string_view to_string(MaintenanceKind value) noexcept {
    for (const auto& entry : kMaintenanceKinds) {
        if (entry.second == value) {
            return entry.first;
        }
    }
    return "unknown";
}

std::string_view to_string(DiagnosticKind value) noexcept {
    switch (value) {
        case DiagnosticKind::kStaleEvidence: return "stale_evidence";
        case DiagnosticKind::kSupersededEvidence: return "superseded_evidence";
        case DiagnosticKind::kConflictingEvidence: return "conflicting_evidence";
        case DiagnosticKind::kIncompleteTopology: return "incomplete_topology";
        case DiagnosticKind::kOutOfRack: return "out_of_rack";
        case DiagnosticKind::kUnknownCapacity: return "unknown_capacity";
        case DiagnosticKind::kCapacityOvercommit: return "capacity_overcommit";
        case DiagnosticKind::kMaintenanceExclusion: return "maintenance_exclusion";
        case DiagnosticKind::kUnsupportedObservation: return "unsupported_observation";
        case DiagnosticKind::kUnknownMember: return "unknown_member";
        case DiagnosticKind::kReplayedEvidence: return "replayed_evidence";
        case DiagnosticKind::kUnresolvedReference: return "unresolved_reference";
    }
    return "unknown";
}

bool lifecycle_transition_allowed(LifecycleState from, LifecycleState to) noexcept {
    if (from == to) {
        return true;  // idempotent re-assertion
    }
    if (from == LifecycleState::kRetired) {
        return false;  // terminal
    }
    switch (from) {
        case LifecycleState::kAssembling:
            return to == LifecycleState::kActive || to == LifecycleState::kDegraded ||
                   to == LifecycleState::kRetired || to == LifecycleState::kRecovering;
        case LifecycleState::kActive:
            return to == LifecycleState::kDegraded || to == LifecycleState::kDraining ||
                   to == LifecycleState::kMaintenance || to == LifecycleState::kRecovering ||
                   to == LifecycleState::kRetired || to == LifecycleState::kAssembling;
        case LifecycleState::kDegraded:
            return to == LifecycleState::kActive || to == LifecycleState::kDraining ||
                   to == LifecycleState::kMaintenance || to == LifecycleState::kRecovering ||
                   to == LifecycleState::kRetired;
        case LifecycleState::kDraining:
            return to == LifecycleState::kMaintenance || to == LifecycleState::kActive ||
                   to == LifecycleState::kDegraded || to == LifecycleState::kRecovering ||
                   to == LifecycleState::kRetired;
        case LifecycleState::kMaintenance:
            return to == LifecycleState::kDraining || to == LifecycleState::kRecovering ||
                   to == LifecycleState::kRetired;
        case LifecycleState::kRecovering:
            return to == LifecycleState::kActive || to == LifecycleState::kDegraded ||
                   to == LifecycleState::kDraining || to == LifecycleState::kMaintenance ||
                   to == LifecycleState::kRetired;
        case LifecycleState::kRetired:
            return false;
    }
    return false;
}

bool parse_device_role(std::string_view text, DeviceRole& out) noexcept {
    return parse_enum(text, out, kDeviceRoles, table_size(kDeviceRoles));
}
bool parse_port_role(std::string_view text, PortRole& out) noexcept {
    return parse_enum(text, out, kPortRoles, table_size(kPortRoles));
}
bool parse_link_kind(std::string_view text, LinkKind& out) noexcept {
    return parse_enum(text, out, kLinkKinds, table_size(kLinkKinds));
}
bool parse_admin_state(std::string_view text, AdminState& out) noexcept {
    return parse_enum(text, out, kAdminStates, table_size(kAdminStates));
}
bool parse_availability(std::string_view text, Availability& out) noexcept {
    return parse_enum(text, out, kAvailabilities, table_size(kAvailabilities));
}
bool parse_lifecycle_state(std::string_view text, LifecycleState& out) noexcept {
    return parse_enum(text, out, kLifecycleStates, table_size(kLifecycleStates));
}
bool parse_resource_kind(std::string_view text, ResourceKind& out) noexcept {
    return parse_enum(text, out, kResourceKinds, table_size(kResourceKinds));
}
bool parse_maintenance_kind(std::string_view text, MaintenanceKind& out) noexcept {
    return parse_enum(text, out, kMaintenanceKinds, table_size(kMaintenanceKinds));
}
bool parse_source_kind(std::string_view text, SourceKind& out) noexcept {
    return parse_enum(text, out, kSourceKinds, table_size(kSourceKinds));
}
bool parse_durability(std::string_view text, Durability& out) noexcept {
    return parse_enum(text, out, kDurabilities, table_size(kDurabilities));
}

Result<ResourceRef> parse_resource_ref(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
        return Status(StatusCode::kMalformedEncoding,
                      "resource reference must be '<kind>:<id>'");
    }
    ResourceKind kind = ResourceKind::kRack;
    if (!parse_resource_kind(text.substr(0, colon), kind)) {
        return Status(StatusCode::kMalformedEncoding, "unknown resource kind");
    }
    bool ok = false;
    const std::uint64_t id = parse_u64(text.substr(colon + 1), ok);
    if (!ok || id == 0) {
        return Status(StatusCode::kInvalidIdentity, "resource id must be a positive integer");
    }
    return ResourceRef{kind, id};
}

}  // namespace rnf
