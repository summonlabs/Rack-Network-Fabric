// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Composition: accepted evidence in, authoritative snapshot out.
//
// Order of work:
//   1. filter each record by applicability to the requested generation
//   2. select the newest applicable observation per subject, deterministically
//   3. build membership, then devices, ports, links and attachments
//   4. apply maintenance exclusions (monotone, replay resistant)
//   5. apply imported obligations (in-rack only)
//   6. derive per resource eligibility and ownership
//   7. close the capacity ledger with checked arithmetic
//   8. enumerate eligible paths
//   9. publish diagnostics and the canonical digest

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "rnf/compose/composer.hpp"

namespace rnf {
namespace {

enum class Applicability : std::uint8_t { kApply, kStale, kFuture };

[[nodiscard]] Applicability applicability_of(const EvidenceRecord& record,
                                             TopologyGeneration target) noexcept {
    if (record.generation.value > target.value) {
        return Applicability::kFuture;
    }
    if (record.durability == Durability::kEphemeral && record.generation.value != target.value) {
        return Applicability::kStale;
    }
    return Applicability::kApply;
}

/// Bounded, canonical diagnostic collector.
class Diagnostics {
public:
    explicit Diagnostics(std::size_t limit) : limit_(limit) {}

    void note(DiagnosticKind kind, EvidenceKind subject_kind, std::uint64_t subject_id,
              SourceId source, std::string detail) {
        if (items_.size() >= limit_) {
            ++dropped_;
            return;
        }
        Diagnostic diagnostic;
        diagnostic.kind = kind;
        diagnostic.subject_kind = subject_kind;
        diagnostic.subject_id = subject_id;
        diagnostic.source = source;
        diagnostic.detail = std::move(detail);
        items_.push_back(std::move(diagnostic));
    }

    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }

    std::vector<Diagnostic> finish() {
        Diagnostic truncation;
        bool has_truncation = dropped_ > 0;
        if (has_truncation) {
            truncation.kind = DiagnosticKind::kIncompleteTopology;
            truncation.subject_kind = EvidenceKind::kMember;
            truncation.detail = "diagnostic list truncated at its configured bound";
        }
        std::sort(items_.begin(), items_.end());
        items_.erase(std::unique(items_.begin(), items_.end()), items_.end());
        if (has_truncation) {
            // Make room for the truncation marker without dropping another entry.
            if (items_.size() >= limit_) {
                items_.pop_back();
            }
            items_.push_back(std::move(truncation));
            std::sort(items_.begin(), items_.end());
        }
        return std::move(items_);
    }

private:
    std::size_t limit_;
    std::size_t dropped_ = 0;
    std::vector<Diagnostic> items_;
};

/// Deterministic newest-wins selection for one subject.
template <class Value>
struct Selection {
    bool present = false;
    bool conflicted = false;
    TopologyGeneration generation{};
    Sequence sequence = 0;
    Digest digest{};
    SourceId source{};
    Value value{};
};

template <class Value>
[[nodiscard]] bool strictly_newer(const Selection<Value>& candidate,
                                  const Selection<Value>& incumbent) noexcept {
    if (candidate.generation.value != incumbent.generation.value) {
        return candidate.generation.value > incumbent.generation.value;
    }
    return candidate.sequence > incumbent.sequence;
}

template <class Value>
[[nodiscard]] bool same_rank(const Selection<Value>& a, const Selection<Value>& b) noexcept {
    return a.generation.value == b.generation.value && a.sequence == b.sequence;
}

template <class Value, class Apply>
void offer(Selection<Value>& slot, Selection<Value> candidate, Apply&& apply) {
    if (!slot.present) {
        slot = std::move(candidate);
        apply(slot);
        return;
    }
    if (strictly_newer(candidate, slot)) {
        const bool was_conflicted = slot.conflicted;
        slot = std::move(candidate);
        slot.conflicted = was_conflicted;
        apply(slot);
        return;
    }
    if (same_rank(candidate, slot)) {
        if (!(candidate.digest == slot.digest)) {
            slot.conflicted = true;
            // Deterministic tie-break: the smaller content digest wins.
            if (candidate.digest < slot.digest) {
                const bool was_conflicted = slot.conflicted;
                slot = std::move(candidate);
                slot.conflicted = was_conflicted;
                apply(slot);
            }
        }
        return;
    }
    // Older than the incumbent: ignored, the incumbent already dominates.
}

[[nodiscard]] constexpr std::uint64_t pack_ref(ResourceKind kind, std::uint64_t id) noexcept {
    return (static_cast<std::uint64_t>(kind) << 56U) | (id & 0x00FFFFFFFFFFFFFFULL);
}

[[nodiscard]] PathId path_identity(const std::vector<LinkId>& links) noexcept {
    Blake2s256 hasher;
    hasher.update("rnf.path.v1");
    for (LinkId link : links) {
        std::uint8_t buf[8];
        for (int i = 0; i < 8; ++i) {
            buf[i] = static_cast<std::uint8_t>((link.value() >> (8 * i)) & 0xFFU);
        }
        hasher.update(ByteSpan(buf, sizeof(buf)));
    }
    const Digest digest = hasher.final();
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(digest.bytes[static_cast<std::size_t>(i)]) << (8 * i);
    }
    return PathId(value == 0 ? 1 : value);
}

[[nodiscard]] constexpr bool is_usable(Availability availability) noexcept {
    return availability == Availability::kUp || availability == Availability::kDegraded;
}

[[nodiscard]] constexpr Capacity min_capacity(Capacity a, Capacity b) noexcept {
    return a.units <= b.units ? a : b;
}

struct MemberInfo {
    bool present = false;
    bool indeterminate = false;
    MemberIncarnation incarnation = 0;
    SourceId source{};
};

struct MaintenanceEntry {
    bool excluded = false;
    MaintenanceKind action = MaintenanceKind::kDrain;
    std::string reason;
    SourceId source{};
    Sequence sequence = 0;
};

struct PathCandidate {
    std::vector<LinkId> links;
    PortId start{};
    PortId end{};
};

}  // namespace

Result<ComposeOutcome> compose(const EvidenceLedger& ledger, const RackConfig& config,
                               const ComposeInput& request) {
    if (!config.rack.is_set()) {
        return Status(StatusCode::kInvalidIdentity, "rack configuration has no rack identity");
    }

    ComposeOutcome outcome;
    ComposeStats& stats = outcome.stats;
    Diagnostics diagnostics(request.limits.max_diagnostics);

    // -- 1/2. membership ----------------------------------------------------
    std::map<DeviceId, MemberInfo> members;
    std::map<DeviceId, Selection<DeviceClaim>> device_claims;
    std::map<PortId, Selection<PortClaim>> port_claims;
    std::map<LinkId, Selection<LinkClaim>> link_claims;
    std::map<AttachmentId, Selection<AttachmentClaim>> attachment_claims;
    std::map<std::uint64_t, std::pair<ResourceRef, MaintenanceEntry>> maintenance;
    std::map<ObligationId, std::pair<ResourceRef, Selection<ObligationClaim>>> obligations;

    for (const auto& [slot, record] : ledger.slots()) {
        (void)slot;
        ++stats.slots_considered;
        const Applicability applicability = applicability_of(record, request.generation);
        if (applicability == Applicability::kFuture) {
            ++stats.slots_stale;
            diagnostics.note(DiagnosticKind::kStaleEvidence, record.kind, slot.subject_a,
                             record.provenance.source,
                             "observation names a later generation than the one being composed");
            continue;
        }
        if (applicability == Applicability::kStale) {
            ++stats.slots_stale;
            diagnostics.note(DiagnosticKind::kStaleEvidence, record.kind, slot.subject_a,
                             record.provenance.source,
                             "ephemeral observation does not apply to this generation");
            continue;
        }
        ++stats.slots_applied;
        const Digest digest = evidence_digest(record);

        switch (record.kind) {
            case EvidenceKind::kMember: {
                const auto& claim = std::get<MemberClaim>(record.payload);
                if (!(claim.rack == config.rack)) {
                    ++stats.slots_out_of_rack;
                    diagnostics.note(DiagnosticKind::kOutOfRack, EvidenceKind::kMember,
                                     claim.device.value(), record.provenance.source,
                                     "membership claim names a different rack");
                    break;
                }
                MemberInfo& info = members[claim.device];
                if (!info.present) {
                    info.present = true;
                    info.incarnation = claim.incarnation;
                    info.source = record.provenance.source;
                } else if (info.incarnation != claim.incarnation) {
                    // Two sources disagree about which incarnation of this
                    // device is currently a member. Neither is preferred: the
                    // device becomes indeterminate and cannot carry authority.
                    info.indeterminate = true;
                    info.incarnation = std::max(info.incarnation, claim.incarnation);
                    diagnostics.note(DiagnosticKind::kConflictingEvidence, EvidenceKind::kMember,
                                     claim.device.value(), record.provenance.source,
                                     "membership incarnation disagrees with another source");
                }
                break;
            }
            case EvidenceKind::kDevice: {
                const auto& claim = std::get<DeviceClaim>(record.payload);
                Selection<DeviceClaim> candidate;
                candidate.present = true;
                candidate.generation = record.generation;
                candidate.sequence = record.provenance.sequence;
                candidate.source = record.provenance.source;
                candidate.digest = digest;
                candidate.value = claim;
                offer(device_claims[claim.device], std::move(candidate), [](Selection<DeviceClaim>&) {});
                break;
            }
            case EvidenceKind::kPort: {
                const auto& claim = std::get<PortClaim>(record.payload);
                Selection<PortClaim> candidate;
                candidate.present = true;
                candidate.generation = record.generation;
                candidate.sequence = record.provenance.sequence;
                candidate.source = record.provenance.source;
                candidate.digest = digest;
                candidate.value = claim;
                offer(port_claims[claim.port], std::move(candidate), [](Selection<PortClaim>&) {});
                break;
            }
            case EvidenceKind::kLink: {
                const auto& claim = std::get<LinkClaim>(record.payload);
                Selection<LinkClaim> candidate;
                candidate.present = true;
                candidate.generation = record.generation;
                candidate.sequence = record.provenance.sequence;
                candidate.source = record.provenance.source;
                candidate.digest = digest;
                candidate.value = claim;
                offer(link_claims[claim.link], std::move(candidate), [](Selection<LinkClaim>&) {});
                break;
            }
            case EvidenceKind::kAttachment: {
                const auto& claim = std::get<AttachmentClaim>(record.payload);
                Selection<AttachmentClaim> candidate;
                candidate.present = true;
                candidate.generation = record.generation;
                candidate.sequence = record.provenance.sequence;
                candidate.source = record.provenance.source;
                candidate.digest = digest;
                candidate.value = claim;
                offer(attachment_claims[claim.attachment], std::move(candidate),
                      [](Selection<AttachmentClaim>&) {});
                break;
            }
            case EvidenceKind::kMaintenance: {
                const auto& claim = std::get<MaintenanceClaim>(record.payload);
                const ResourceRef target{claim.target_kind, claim.target_id};
                auto& entry = maintenance[pack_ref(claim.target_kind, claim.target_id)];
                entry.first = target;
                // One ledger slot exists per (target, source), and the ledger
                // keeps the newest record for that slot. Any source whose newest
                // directive is a drain or a maintenance window therefore keeps
                // the exclusion alive, and an older replay cannot clear it.
                if (claim.action != MaintenanceKind::kClear) {
                    if (!entry.second.excluded ||
                        record.provenance.sequence >= entry.second.sequence) {
                        entry.second.excluded = true;
                        entry.second.action = claim.action;
                        entry.second.reason = claim.reason;
                        entry.second.source = record.provenance.source;
                        entry.second.sequence = record.provenance.sequence;
                    }
                }
                break;
            }
            case EvidenceKind::kObligation: {
                const auto& claim = std::get<ObligationClaim>(record.payload);
                Selection<ObligationClaim> candidate;
                candidate.present = true;
                candidate.generation = record.generation;
                candidate.sequence = record.provenance.sequence;
                candidate.source = record.provenance.source;
                candidate.digest = digest;
                candidate.value = claim;
                auto& entry = obligations[claim.obligation];
                entry.first = ResourceRef{claim.target_kind, claim.target_id};
                offer(entry.second, std::move(candidate), [](Selection<ObligationClaim>&) {});
                break;
            }
        }
    }

    // -- 3. membership table ------------------------------------------------
    if (members.size() > request.limits.max_devices) {
        return Status(StatusCode::kResourceExhausted, "member count exceeds the compose bound");
    }

    SnapshotData data;
    data.rack = config.rack;
    data.generation = request.generation;
    data.epoch = request.epoch;
    data.lifecycle = request.lifecycle;
    data.evidence_digest = ledger.digest();

    std::map<DeviceId, MemberIncarnation> member_incarnations;
    for (const auto& [device, info] : members) {
        member_incarnations[device] = info.incarnation;
    }

    // Maintenance exclusions, resolved by target.
    const auto rack_maintenance = maintenance.find(pack_ref(ResourceKind::kRack, 0));
    const bool rack_excluded =
        rack_maintenance != maintenance.end() && rack_maintenance->second.second.excluded;

    auto exclusion_of = [&](ResourceKind kind, std::uint64_t id) -> bool {
        if (rack_excluded) {
            return true;
        }
        const auto it = maintenance.find(pack_ref(kind, id));
        return it != maintenance.end() && it->second.second.excluded;
    };

    // -- devices ------------------------------------------------------------
    std::map<DeviceId, bool> device_excluded;
    for (const auto& [device, info] : members) {
        (void)info;
        device_excluded[device] = exclusion_of(ResourceKind::kDevice, device.value());
    }

    std::size_t duplicate_port_keys = 0;
    std::map<std::pair<std::uint64_t, std::uint16_t>, PortId> port_keys;

    for (const auto& [device, selection] : device_claims) {
        const auto member = members.find(device);
        if (member == members.end()) {
            diagnostics.note(DiagnosticKind::kUnknownMember, EvidenceKind::kDevice,
                             device.value(), selection.source,
                             "device observation has no matching membership claim");
            continue;
        }
        if (selection.conflicted) {
            diagnostics.note(DiagnosticKind::kConflictingEvidence, EvidenceKind::kDevice,
                             device.value(), selection.source,
                             "sources disagree about this device at the same generation");
        }
        if (member->second.indeterminate) {
            diagnostics.note(DiagnosticKind::kConflictingEvidence, EvidenceKind::kDevice,
                             device.value(), selection.source,
                             "membership incarnation is indeterminate");
        }

        DeviceRecord record;
        record.id = device;
        record.role = selection.value.role;
        record.incarnation = member->second.incarnation;
        record.capacity = selection.value.capacity;
        record.capacity_known = selection.value.capacity_known;
        record.availability = selection.value.availability;

        if (!record.capacity_known) {
            diagnostics.note(DiagnosticKind::kUnknownCapacity, EvidenceKind::kDevice,
                             device.value(), selection.source,
                             "device capacity was not measured");
        }
        const bool excluded = device_excluded[device];
        if (excluded) {
            diagnostics.note(DiagnosticKind::kMaintenanceExclusion, EvidenceKind::kDevice,
                             device.value(), selection.source,
                             "device is inside a drain or maintenance window");
        }
        const bool conflicted = selection.conflicted || member->second.indeterminate;
        if (conflicted || excluded || !record.capacity_known || !is_usable(record.availability)) {
            record.eligibility = conflicted ? Eligibility::kIncomplete
                                            : (excluded ? Eligibility::kIneligible
                                                        : Eligibility::kIneligible);
        } else {
            record.eligibility = Eligibility::kEligible;
        }
        data.devices.push_back(std::move(record));
    }

    // Devices that are members but have no observation at all are reported and
    // excluded: membership alone does not make a device usable.
    for (const auto& [device, info] : members) {
        const bool observed = device_claims.find(device) != device_claims.end();
        if (!observed) {
            diagnostics.note(DiagnosticKind::kIncompleteTopology, EvidenceKind::kDevice,
                             device.value(), info.source,
                             "member device has no device observation");
            DeviceRecord record;
            record.id = device;
            record.incarnation = info.incarnation;
            record.eligibility = Eligibility::kIncomplete;
            data.devices.push_back(std::move(record));
        }
    }
    std::sort(data.devices.begin(), data.devices.end(),
              [](const DeviceRecord& a, const DeviceRecord& b) { return a.id < b.id; });
    data.devices.erase(std::unique(data.devices.begin(), data.devices.end(),
                                   [](const DeviceRecord& a, const DeviceRecord& b) {
                                       return a.id == b.id;
                                   }),
                       data.devices.end());
    if (data.devices.size() > request.limits.max_devices) {
        return Status(StatusCode::kResourceExhausted, "device count exceeds the compose bound");
    }
    stats.member_devices = data.devices.size();

    auto device_record = [&](DeviceId id) -> const DeviceRecord* {
        const auto it = std::lower_bound(
            data.devices.begin(), data.devices.end(), id,
            [](const DeviceRecord& record, DeviceId value) { return record.id < value; });
        if (it == data.devices.end() || !(it->id == id)) {
            return nullptr;
        }
        return &*it;
    };

    // -- ports --------------------------------------------------------------
    for (const auto& [port, selection] : port_claims) {
        const auto device_it = members.find(selection.value.device);
        if (device_it == members.end()) {
            diagnostics.note(DiagnosticKind::kOutOfRack, EvidenceKind::kPort, port.value(),
                             selection.source,
                             "port belongs to a device that is not a member of this rack");
            continue;
        }
        if (selection.conflicted) {
            diagnostics.note(DiagnosticKind::kConflictingEvidence, EvidenceKind::kPort,
                             port.value(), selection.source,
                             "sources disagree about this port at the same generation");
        }
        const auto key = std::make_pair(selection.value.device.value(), selection.value.index);
        const auto existing = port_keys.find(key);
        if (existing != port_keys.end() && !(existing->second == port)) {
            ++duplicate_port_keys;
            diagnostics.note(DiagnosticKind::kConflictingEvidence, EvidenceKind::kPort,
                             port.value(), selection.source,
                             "two port identities claim the same device/index pair");
        } else {
            port_keys.emplace(key, port);
        }

        PortRecord record;
        record.id = port;
        record.device = selection.value.device;
        record.index = selection.value.index;
        record.role = selection.value.role;
        record.capacity = selection.value.capacity;
        record.capacity_known = selection.value.capacity_known;
        record.availability = selection.value.availability;
        record.admin = selection.value.admin;

        const bool excluded = device_excluded[record.device] ||
                              exclusion_of(ResourceKind::kPort, port.value());
        if (excluded) {
            diagnostics.note(DiagnosticKind::kMaintenanceExclusion, EvidenceKind::kPort,
                             port.value(), selection.source,
                             "port is inside a drain or maintenance window");
        }
        if (!record.capacity_known) {
            diagnostics.note(DiagnosticKind::kUnknownCapacity, EvidenceKind::kPort, port.value(),
                             selection.source, "port capacity was not measured");
        }
        // A port cannot be usable when the device that owns it is not. Without
        // this rule a port on a device whose membership is indeterminate would
        // still hand out capacity.
        const DeviceRecord* owner = device_record(record.device);
        const bool owner_usable =
            owner != nullptr && owner->eligibility == Eligibility::kEligible;
        if (selection.conflicted) {
            record.eligibility = Eligibility::kIncomplete;
        } else if (excluded || !owner_usable) {
            record.eligibility = Eligibility::kIneligible;
        } else if (!record.capacity_known || !is_usable(record.availability) ||
                   record.admin != AdminState::kEnabled) {
            record.eligibility = Eligibility::kIneligible;
        } else {
            record.eligibility = Eligibility::kEligible;
        }
        data.ports.push_back(std::move(record));
    }
    std::sort(data.ports.begin(), data.ports.end(),
              [](const PortRecord& a, const PortRecord& b) { return a.id < b.id; });
    if (data.ports.size() > request.limits.max_ports) {
        return Status(StatusCode::kResourceExhausted, "port count exceeds the compose bound");
    }

    auto port_record = [&](PortId id) -> const PortRecord* {
        const auto it = std::lower_bound(
            data.ports.begin(), data.ports.end(), id,
            [](const PortRecord& record, PortId value) { return record.id < value; });
        if (it == data.ports.end() || !(it->id == id)) {
            return nullptr;
        }
        return &*it;
    };

    // -- links --------------------------------------------------------------
    for (const auto& [link, selection] : link_claims) {
        LinkRecord record;
        record.id = link;
        record.port_a = selection.value.port_a;
        record.port_b = selection.value.port_b;
        record.kind = selection.value.kind;
        record.capacity = selection.value.capacity;
        record.capacity_known = selection.value.capacity_known;
        record.availability = selection.value.availability;

        const PortRecord* a = port_record(record.port_a);
        const PortRecord* b = port_record(record.port_b);
        if (a == nullptr || b == nullptr) {
            diagnostics.note(DiagnosticKind::kIncompleteTopology, EvidenceKind::kLink, link.value(),
                             selection.source, "link endpoint is not a known port in this rack");
            record.eligibility = Eligibility::kIncomplete;
            data.links.push_back(std::move(record));
            continue;
        }
        if (selection.conflicted) {
            diagnostics.note(DiagnosticKind::kConflictingEvidence, EvidenceKind::kLink,
                             link.value(), selection.source,
                             "sources disagree about this link at the same generation");
        }
        const bool excluded = device_excluded[a->device] || device_excluded[b->device] ||
                              exclusion_of(ResourceKind::kPort, a->id.value()) ||
                              exclusion_of(ResourceKind::kPort, b->id.value()) ||
                              exclusion_of(ResourceKind::kLink, link.value());
        if (excluded) {
            diagnostics.note(DiagnosticKind::kMaintenanceExclusion, EvidenceKind::kLink,
                             link.value(), selection.source,
                             "link touches a resource inside a drain or maintenance window");
        }
        record.availability = combine_availability(
            combine_availability(record.availability, a->availability), b->availability);
        if (!record.capacity_known) {
            diagnostics.note(DiagnosticKind::kUnknownCapacity, EvidenceKind::kLink, link.value(),
                             selection.source, "link capacity was not measured");
        }
        // A link can never carry more than its weakest endpoint port.
        if (a->capacity_known && b->capacity_known) {
            record.capacity = min_capacity(min_capacity(record.capacity, a->capacity), b->capacity);
        } else {
            record.capacity_known = false;
        }

        if (selection.conflicted) {
            record.eligibility = Eligibility::kIncomplete;
        } else if (excluded) {
            record.eligibility = Eligibility::kIneligible;
        } else if (!record.capacity_known || !is_usable(record.availability) ||
                   a->eligibility != Eligibility::kEligible ||
                   b->eligibility != Eligibility::kEligible) {
            record.eligibility = Eligibility::kIneligible;
        } else {
            record.eligibility = Eligibility::kEligible;
        }
        data.links.push_back(std::move(record));
    }
    std::sort(data.links.begin(), data.links.end(),
              [](const LinkRecord& a, const LinkRecord& b) { return a.id < b.id; });
    if (data.links.size() > request.limits.max_links) {
        return Status(StatusCode::kResourceExhausted, "link count exceeds the compose bound");
    }

    auto link_record = [&](LinkId id) -> const LinkRecord* {
        const auto it = std::lower_bound(
            data.links.begin(), data.links.end(), id,
            [](const LinkRecord& record, LinkId value) { return record.id < value; });
        if (it == data.links.end() || !(it->id == id)) {
            return nullptr;
        }
        return &*it;
    };

    // -- attachments --------------------------------------------------------
    for (const auto& [attachment, selection] : attachment_claims) {
        AttachmentRecord record;
        record.id = attachment;
        record.device = selection.value.device;
        record.port = selection.value.port;
        record.host_key = selection.value.host_key;

        const DeviceRecord* device = device_record(record.device);
        const PortRecord* port = port_record(record.port);
        if (device == nullptr) {
            diagnostics.note(DiagnosticKind::kOutOfRack, EvidenceKind::kAttachment,
                             attachment.value(), selection.source,
                             "attachment names a device that is not a member of this rack");
            record.eligibility = Eligibility::kIncomplete;
            data.attachments.push_back(std::move(record));
            continue;
        }
        if (port == nullptr) {
            diagnostics.note(DiagnosticKind::kUnresolvedReference, EvidenceKind::kAttachment,
                             attachment.value(), selection.source,
                             "attachment names a port that is not known");
            record.eligibility = Eligibility::kIncomplete;
            data.attachments.push_back(std::move(record));
            continue;
        }
        if (!(port->device == record.device)) {
            diagnostics.note(DiagnosticKind::kConflictingEvidence, EvidenceKind::kAttachment,
                             attachment.value(), selection.source,
                             "attachment device does not own the named port");
            record.eligibility = Eligibility::kIncomplete;
            data.attachments.push_back(std::move(record));
            continue;
        }
        if (selection.conflicted) {
            diagnostics.note(DiagnosticKind::kConflictingEvidence, EvidenceKind::kAttachment,
                             attachment.value(), selection.source,
                             "sources disagree about this attachment at the same generation");
            record.eligibility = Eligibility::kIncomplete;
        } else if (device->eligibility != Eligibility::kEligible ||
                   port->eligibility != Eligibility::kEligible) {
            record.eligibility = Eligibility::kIneligible;
        } else {
            record.eligibility = Eligibility::kEligible;
        }
        data.attachments.push_back(std::move(record));
    }
    std::sort(data.attachments.begin(), data.attachments.end(),
              [](const AttachmentRecord& a, const AttachmentRecord& b) { return a.id < b.id; });
    if (data.attachments.size() > request.limits.max_attachments) {
        return Status(StatusCode::kResourceExhausted,
                      "attachment count exceeds the compose bound");
    }

    // -- maintenance records ------------------------------------------------
    for (const auto& [key, entry] : maintenance) {
        (void)key;
        if (!entry.second.excluded) {
            continue;
        }
        MaintenanceRecord record;
        record.action = entry.second.action;
        record.target = entry.first;
        record.reason = entry.second.reason;
        record.source = entry.second.source;
        record.sequence = entry.second.sequence;
        data.maintenance.push_back(std::move(record));
    }
    std::sort(data.maintenance.begin(), data.maintenance.end(),
              [](const MaintenanceRecord& a, const MaintenanceRecord& b) {
                  if (!(a.target == b.target)) {
                      return a.target < b.target;
                  }
                  return a.source < b.source;
              });
    if (data.maintenance.size() > request.limits.max_maintenance) {
        return Status(StatusCode::kResourceExhausted,
                      "maintenance record count exceeds the compose bound");
    }

    // -- obligations --------------------------------------------------------
    std::map<ResourceRef, Capacity> obligated_by_target;
    for (const auto& [id, entry] : obligations) {
        (void)id;
        const ResourceRef target = entry.first;
        const Selection<ObligationClaim>& selection = entry.second;
        const bool target_in_rack =
            (target.kind == ResourceKind::kRack && target.id == config.rack.value()) ||
            (target.kind == ResourceKind::kRack && target.id == 0) ||
            (target.kind == ResourceKind::kDevice &&
             members.find(DeviceId(target.id)) != members.end()) ||
            (target.kind == ResourceKind::kPort && port_record(PortId(target.id)) != nullptr) ||
            (target.kind == ResourceKind::kLink && link_record(LinkId(target.id)) != nullptr) ||
            (target.kind == ResourceKind::kAttachment &&
             attachment_claims.find(AttachmentId(target.id)) != attachment_claims.end());
        if (!target_in_rack) {
            ++stats.slots_out_of_rack;
            diagnostics.note(DiagnosticKind::kOutOfRack, EvidenceKind::kObligation, target.id,
                             selection.source,
                             "imported obligation names a resource outside this rack");
            continue;
        }
        ObligationRecord record;
        record.id = ObligationId(id.value());
        record.target = target;
        record.capacity = selection.value.capacity;
        record.holder = selection.value.holder;
        record.authority = selection.value.authority;
        record.source = selection.source;
        data.obligations.push_back(std::move(record));

        auto& bucket = obligated_by_target[target];
        std::uint64_t sum = 0;
        if (!checked_add_u64(bucket.units, record.capacity.units, sum)) {
            return Status(StatusCode::kOutOfRange, "obligation capacity overflowed");
        }
        bucket.units = sum;
    }
    std::sort(data.obligations.begin(), data.obligations.end(),
              [](const ObligationRecord& a, const ObligationRecord& b) { return a.id < b.id; });
    if (data.obligations.size() > request.limits.max_obligations) {
        return Status(StatusCode::kResourceExhausted,
                      "obligation count exceeds the compose bound");
    }

    // -- capacity ledger ----------------------------------------------------
    Capacity total{};
    Capacity unavailable{};
    for (const DeviceRecord& device : data.devices) {
        if (!device.capacity_known) {
            continue;
        }
        std::uint64_t next_total = 0;
        if (!checked_add_u64(total.units, device.capacity.units, next_total)) {
            return Status(StatusCode::kOutOfRange, "device capacity sum overflowed");
        }
        total.units = next_total;
        if (device.eligibility != Eligibility::kEligible || !is_usable(device.availability)) {
            std::uint64_t next_unavailable = 0;
            if (!checked_add_u64(unavailable.units, device.capacity.units, next_unavailable)) {
                return Status(StatusCode::kOutOfRange, "unavailable capacity sum overflowed");
            }
            unavailable.units = next_unavailable;
        }
    }
    std::uint64_t usable_units = 0;
    if (!checked_sub_u64(total.units, unavailable.units, usable_units)) {
        return Status(StatusCode::kIntegrityFailure, "capacity accounting underflowed");
    }

    Capacity obligated{};
    for (const auto& [target, amount] : obligated_by_target) {
        (void)target;
        std::uint64_t sum = 0;
        if (!checked_add_u64(obligated.units, amount.units, sum)) {
            return Status(StatusCode::kOutOfRange, "obligation sum overflowed");
        }
        obligated.units = sum;
    }

    data.capacity.total = total;
    data.capacity.unavailable = unavailable;
    data.capacity.usable = Capacity{usable_units};
    data.capacity.obligated = obligated;
    data.capacity.headroom_floor = config.headroom_floor;

    std::uint64_t claimed = 0;
    if (!checked_add_u64(obligated.units, config.headroom_floor.units, claimed)) {
        return Status(StatusCode::kOutOfRange, "headroom plus obligations overflowed");
    }
    if (claimed > usable_units) {
        data.capacity.deficit = Capacity{claimed - usable_units};
        data.capacity.uncommitted = Capacity{0};
        diagnostics.note(DiagnosticKind::kCapacityOvercommit, EvidenceKind::kMember, 0, SourceId{},
                         "obligations and reserved headroom exceed usable rack capacity");
    } else {
        data.capacity.deficit = Capacity{0};
        data.capacity.uncommitted = Capacity{usable_units - claimed};
    }
    if (!data.capacity.closes()) {
        return Status(StatusCode::kIntegrityFailure, "composed capacity ledger does not close");
    }

    // -- paths --------------------------------------------------------------
    std::map<PortId, std::vector<std::pair<LinkId, PortId>>> adjacency;
    for (const LinkRecord& link : data.links) {
        if (link.eligibility != Eligibility::kEligible) {
            continue;
        }
        adjacency[link.port_a].emplace_back(link.id, link.port_b);
        adjacency[link.port_b].emplace_back(link.id, link.port_a);
    }
    for (auto& [port, neighbours] : adjacency) {
        (void)port;
        std::sort(neighbours.begin(), neighbours.end());
    }

    std::set<PortId> access_ports;
    for (const AttachmentRecord& attachment : data.attachments) {
        if (attachment.eligibility == Eligibility::kEligible) {
            access_ports.insert(attachment.port);
        }
    }
    std::set<PortId> exit_ports;
    for (const PortRecord& port : data.ports) {
        if (port.role == PortRole::kUplink || port.role == PortRole::kFabric ||
            port.role == PortRole::kPeer) {
            exit_ports.insert(port.id);
        }
    }

    std::vector<PathCandidate> candidates;
    std::size_t expansions = 0;
    const std::size_t max_expansions = 1U << 18;
    bool truncated = false;

    for (PortId start : access_ports) {
        std::vector<LinkId> stack;
        std::set<PortId> visited;
        std::map<PortId, std::size_t> found_per_endpoint;

        // Iterative depth first search over eligible links.
        struct Frame {
            PortId port;
            std::size_t next_neighbour = 0;
        };
        std::vector<Frame> frames;
        visited.insert(start);
        frames.push_back(Frame{start, 0});

        while (!frames.empty()) {
            if (++expansions > max_expansions) {
                truncated = true;
                break;
            }
            Frame& frame = frames.back();
            const auto neighbours = adjacency.find(frame.port);
            const std::size_t degree =
                neighbours == adjacency.end() ? 0 : neighbours->second.size();
            if (frame.next_neighbour >= degree) {
                visited.erase(frame.port);
                frames.pop_back();
                if (!stack.empty()) {
                    stack.pop_back();
                }
                continue;
            }
            const auto [link_id, next_port] = neighbours->second[frame.next_neighbour++];
            if (visited.count(next_port) != 0) {
                continue;
            }
            if (stack.size() >= config.max_path_hops) {
                truncated = true;
                continue;
            }
            PathCandidate candidate;
            candidate.links = stack;
            candidate.links.push_back(link_id);
            candidate.start = start;
            candidate.end = next_port;
            const bool is_exit = exit_ports.count(next_port) != 0;
            if (is_exit) {
                std::size_t& count = found_per_endpoint[next_port];
                if (count < config.max_paths_per_pair) {
                    ++count;
                    candidates.push_back(std::move(candidate));
                } else {
                    truncated = true;
                }
                if (candidates.size() > request.limits.max_paths) {
                    return Status(StatusCode::kResourceExhausted,
                                  "path count exceeds the compose bound");
                }
                continue;
            }
            visited.insert(next_port);
            stack.push_back(link_id);
            frames.push_back(Frame{next_port, 0});
        }
        if (truncated) {
            break;
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const PathCandidate& a, const PathCandidate& b) {
                  if (!(a.start == b.start)) {
                      return a.start < b.start;
                  }
                  if (!(a.end == b.end)) {
                      return a.end < b.end;
                  }
                  return a.links < b.links;
              });
    candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                 [](const PathCandidate& a, const PathCandidate& b) {
                                     return a.links == b.links;
                                 }),
                     candidates.end());
    if (candidates.size() > request.limits.max_paths) {
        candidates.resize(request.limits.max_paths);
        truncated = true;
    }
    if (truncated) {
        diagnostics.note(DiagnosticKind::kIncompleteTopology, EvidenceKind::kLink, 0, SourceId{},
                         "path enumeration was truncated at a configured bound");
    }

    for (const PathCandidate& candidate : candidates) {
        PathRecord path;
        path.id = path_identity(candidate.links);
        path.start_port = candidate.start;
        path.end_port = candidate.end;
        path.links = candidate.links;
        bool known = true;
        Capacity bottleneck{};
        bool first = true;
        for (LinkId link_id : candidate.links) {
            const LinkRecord* link = link_record(link_id);
            if (link == nullptr || link->eligibility != Eligibility::kEligible) {
                known = false;
                break;
            }
            if (!link->capacity_known) {
                known = false;
                break;
            }
            bottleneck = first ? link->capacity : min_capacity(bottleneck, link->capacity);
            first = false;
        }
        path.bottleneck_known = known;
        path.bottleneck = bottleneck;
        path.eligibility = known ? Eligibility::kEligible : Eligibility::kIncomplete;
        if (!known) {
            diagnostics.note(DiagnosticKind::kUnknownCapacity, EvidenceKind::kLink, path.id.value(),
                             SourceId{}, "path contains a link whose capacity is not known");
        }
        data.paths.push_back(std::move(path));
    }
    std::sort(data.paths.begin(), data.paths.end(), [](const PathRecord& a, const PathRecord& b) {
        if (!(a.start_port == b.start_port)) {
            return a.start_port < b.start_port;
        }
        if (!(a.end_port == b.end_port)) {
            return a.end_port < b.end_port;
        }
        return a.links < b.links;
    });
    stats.paths_enumerated = data.paths.size();

    // -- resource states ----------------------------------------------------
    auto add_resource = [&](ResourceRef ref, Eligibility eligibility, Availability availability,
                            bool capacity_known, Capacity capacity, Capacity obligated_amount,
                            bool excluded, std::vector<OwnerRef> owners) {
        ResourceState state;
        state.ref = ref;
        state.eligibility = eligibility;
        state.availability = availability;
        state.capacity_known = capacity_known;
        state.capacity = capacity;
        state.obligated = obligated_amount;
        state.maintenance = excluded;
        std::sort(owners.begin(), owners.end());
        owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
        state.owners = std::move(owners);
        data.resources.push_back(std::move(state));
    };

    std::vector<OwnerRef> rack_owners;
    rack_owners.reserve(data.devices.size());
    for (const DeviceRecord& device : data.devices) {
        rack_owners.push_back(OwnerRef{device.id, device.incarnation});
    }
    const auto rack_obligation = obligated_by_target.find(ResourceRef{ResourceKind::kRack, 0});
    add_resource(ResourceRef{ResourceKind::kRack, config.rack.value()},
                 rack_excluded ? Eligibility::kIneligible : Eligibility::kEligible,
                 Availability::kUp, true, total,
                 rack_obligation == obligated_by_target.end() ? Capacity{} : rack_obligation->second,
                 rack_excluded, rack_owners);

    for (const DeviceRecord& device : data.devices) {
        const auto bucket = obligated_by_target.find(ResourceRef{ResourceKind::kDevice,
                                                                  device.id.value()});
        add_resource(ResourceRef{ResourceKind::kDevice, device.id.value()}, device.eligibility,
                     device.availability, device.capacity_known, device.capacity,
                     bucket == obligated_by_target.end() ? Capacity{} : bucket->second,
                     device_excluded[device.id], {OwnerRef{device.id, device.incarnation}});
    }
    for (const PortRecord& port : data.ports) {
        const auto bucket = obligated_by_target.find(ResourceRef{ResourceKind::kPort,
                                                                  port.id.value()});
        const auto device = device_record(port.device);
        add_resource(ResourceRef{ResourceKind::kPort, port.id.value()}, port.eligibility,
                     port.availability, port.capacity_known, port.capacity,
                     bucket == obligated_by_target.end() ? Capacity{} : bucket->second,
                     device_excluded[port.device] ||
                         exclusion_of(ResourceKind::kPort, port.id.value()),
                     device == nullptr
                         ? std::vector<OwnerRef>{}
                         : std::vector<OwnerRef>{OwnerRef{device->id, device->incarnation}});
    }
    for (const LinkRecord& link : data.links) {
        const auto bucket = obligated_by_target.find(ResourceRef{ResourceKind::kLink,
                                                                  link.id.value()});
        std::vector<OwnerRef> owners;
        const PortRecord* a = port_record(link.port_a);
        const PortRecord* b = port_record(link.port_b);
        if (a != nullptr) {
            const DeviceRecord* device = device_record(a->device);
            if (device != nullptr) {
                owners.push_back(OwnerRef{device->id, device->incarnation});
            }
        }
        if (b != nullptr) {
            const DeviceRecord* device = device_record(b->device);
            if (device != nullptr) {
                owners.push_back(OwnerRef{device->id, device->incarnation});
            }
        }
        const bool excluded =
            (a != nullptr && device_excluded[a->device]) || (b != nullptr && device_excluded[b->device]) ||
            exclusion_of(ResourceKind::kLink, link.id.value());
        add_resource(ResourceRef{ResourceKind::kLink, link.id.value()}, link.eligibility,
                     link.availability, link.capacity_known, link.capacity,
                     bucket == obligated_by_target.end() ? Capacity{} : bucket->second, excluded,
                     std::move(owners));
    }
    for (const AttachmentRecord& attachment : data.attachments) {
        const auto bucket = obligated_by_target.find(ResourceRef{ResourceKind::kAttachment,
                                                                  attachment.id.value()});
        const DeviceRecord* device = device_record(attachment.device);
        const PortRecord* bound_port = port_record(attachment.port);
        // An attachment is a binding, not a conduit, so it has no capacity of
        // its own. It can carry exactly what the port it binds to can carry,
        // and it is unknown only when that port's capacity is unknown.
        bool attachment_capacity_known = false;
        Capacity attachment_capacity{};
        if (bound_port != nullptr && bound_port->capacity_known) {
            attachment_capacity = bound_port->capacity;
            attachment_capacity_known = true;
            if (device != nullptr && device->capacity_known) {
                attachment_capacity =
                    min_capacity(attachment_capacity, device->capacity);
            }
        }
        add_resource(ResourceRef{ResourceKind::kAttachment, attachment.id.value()},
                     attachment.eligibility, Availability::kUp, attachment_capacity_known,
                     attachment_capacity,
                     bucket == obligated_by_target.end() ? Capacity{} : bucket->second,
                     device_excluded[attachment.device],
                     device == nullptr
                         ? std::vector<OwnerRef>{}
                         : std::vector<OwnerRef>{OwnerRef{device->id, device->incarnation}});
    }
    for (const PathRecord& path : data.paths) {
        const auto bucket = obligated_by_target.find(ResourceRef{ResourceKind::kPath,
                                                                  path.id.value()});
        std::vector<OwnerRef> owners;
        for (LinkId link_id : path.links) {
            const LinkRecord* link = link_record(link_id);
            if (link == nullptr) {
                continue;
            }
            const PortRecord* a = port_record(link->port_a);
            const PortRecord* b = port_record(link->port_b);
            if (a != nullptr) {
                const DeviceRecord* device = device_record(a->device);
                if (device != nullptr) {
                    owners.push_back(OwnerRef{device->id, device->incarnation});
                }
            }
            if (b != nullptr) {
                const DeviceRecord* device = device_record(b->device);
                if (device != nullptr) {
                    owners.push_back(OwnerRef{device->id, device->incarnation});
                }
            }
        }
        add_resource(ResourceRef{ResourceKind::kPath, path.id.value()}, path.eligibility,
                     Availability::kUp, path.bottleneck_known, path.bottleneck,
                     bucket == obligated_by_target.end() ? Capacity{} : bucket->second, false,
                     std::move(owners));
    }

    std::sort(data.resources.begin(), data.resources.end(),
              [](const ResourceState& a, const ResourceState& b) { return a.ref < b.ref; });
    data.resources.erase(std::unique(data.resources.begin(), data.resources.end(),
                                     [](const ResourceState& a, const ResourceState& b) {
                                         return a.ref == b.ref;
                                     }),
                         data.resources.end());

    // -- diagnostics and digest --------------------------------------------
    data.diagnostics = diagnostics.finish();
    stats.diagnostics = data.diagnostics.size();
    data.member_set_digest = member_set_digest(data.devices);

    auto shared = std::make_shared<SnapshotData>(std::move(data));
    const Digest digest = snapshot_digest(*shared);
    outcome.snapshot = Snapshot(std::move(shared), digest);
    return outcome;
}

}  // namespace rnf
