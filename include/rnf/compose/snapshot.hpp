// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// An immutable, canonical, content-addressed view of one rack at one
// generation. Two runs that accept equivalent evidence produce byte-identical
// canonical encodings and therefore the same digest, regardless of the order in
// which that evidence arrived.

#ifndef RNF_COMPOSE_SNAPSHOT_HPP
#define RNF_COMPOSE_SNAPSHOT_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rnf/core/bytes.hpp"
#include "rnf/model/evidence.hpp"
#include "rnf/model/types.hpp"

namespace rnf {

/// Bounds on a composed snapshot. A composition that would exceed any of these
/// fails with kResourceExhausted rather than producing a partial snapshot.
struct ComposeLimits {
    std::size_t max_devices = 4096;
    std::size_t max_ports = 16384;
    std::size_t max_links = 16384;
    std::size_t max_attachments = 16384;
    std::size_t max_paths = 4096;
    std::size_t max_diagnostics = 4096;
    std::size_t max_obligations = 8192;
    std::size_t max_maintenance = 8192;
    std::size_t max_encoded_snapshot = 16U << 20;  // 16 MiB
};

[[nodiscard]] const ComposeLimits& default_compose_limits() noexcept;

/// One device that owns, or participates in, a resource.
struct OwnerRef {
    DeviceId device{};
    MemberIncarnation incarnation = 0;
    friend constexpr bool operator==(const OwnerRef&, const OwnerRef&) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(const OwnerRef& a, const OwnerRef& b) noexcept {
        if (a.device != b.device) {
            return a.device <=> b.device;
        }
        return a.incarnation <=> b.incarnation;
    }
};

/// The authoritative view of a single resource.
struct ResourceState {
    ResourceRef ref{};
    Eligibility eligibility = Eligibility::kUnknown;
    Availability availability = Availability::kUnknown;
    bool capacity_known = false;
    Capacity capacity{};
    Capacity obligated{};
    bool maintenance = false;
    /// Every device whose incarnation the resource's usability depends on. A
    /// change to any of these fences authority that named this resource.
    std::vector<OwnerRef> owners;

    friend bool operator==(const ResourceState&, const ResourceState&) = default;
};

struct DeviceRecord {
    DeviceId id{};
    DeviceRole role = DeviceRole::kUnknown;
    MemberIncarnation incarnation = 0;
    Capacity capacity{};
    bool capacity_known = false;
    Eligibility eligibility = Eligibility::kUnknown;
    Availability availability = Availability::kUnknown;
    friend bool operator==(const DeviceRecord&, const DeviceRecord&) = default;
};

struct PortRecord {
    PortId id{};
    DeviceId device{};
    std::uint16_t index = 0;
    PortRole role = PortRole::kUnknown;
    Capacity capacity{};
    bool capacity_known = false;
    Availability availability = Availability::kUnknown;
    AdminState admin = AdminState::kUnknown;
    Eligibility eligibility = Eligibility::kUnknown;
    friend bool operator==(const PortRecord&, const PortRecord&) = default;
};

struct LinkRecord {
    LinkId id{};
    PortId port_a{};
    PortId port_b{};
    LinkKind kind = LinkKind::kUnknown;
    Capacity capacity{};
    bool capacity_known = false;
    Availability availability = Availability::kUnknown;
    Eligibility eligibility = Eligibility::kUnknown;
    friend bool operator==(const LinkRecord&, const LinkRecord&) = default;
};

struct AttachmentRecord {
    AttachmentId id{};
    DeviceId device{};
    PortId port{};
    std::string host_key;
    Eligibility eligibility = Eligibility::kUnknown;
    friend bool operator==(const AttachmentRecord&, const AttachmentRecord&) = default;
};

/// A simple, bounded chain of links between two ports of the rack.
struct PathRecord {
    PathId id{};
    PortId start_port{};
    PortId end_port{};
    std::vector<LinkId> links;
    Capacity bottleneck{};
    bool bottleneck_known = false;
    Eligibility eligibility = Eligibility::kUnknown;
    friend bool operator==(const PathRecord&, const PathRecord&) = default;
};

struct MaintenanceRecord {
    MaintenanceKind action = MaintenanceKind::kDrain;
    ResourceRef target{};
    std::string reason;
    SourceId source{};
    Sequence sequence = 0;
    friend bool operator==(const MaintenanceRecord&, const MaintenanceRecord&) = default;
};

struct ObligationRecord {
    ObligationId id{};
    ResourceRef target{};
    Capacity capacity{};
    PrincipalId holder{};
    std::string authority;
    SourceId source{};
    friend bool operator==(const ObligationRecord&, const ObligationRecord&) = default;
};

/// Rack capacity accounting. Both identities hold exactly, by construction, and
/// are re-checked with checked arithmetic before a snapshot is published:
///
///   total == unavailable + usable
///   usable + deficit == obligated + headroom_floor + uncommitted
///
/// deficit is non-zero only when maintenance, a member departure or a new
/// imported obligation removed capacity that already issued authority depends
/// on. It is reported, never hidden by clamping.
struct CapacityLedger {
    Capacity total{};
    Capacity unavailable{};
    Capacity usable{};
    Capacity obligated{};
    Capacity headroom_floor{};
    Capacity uncommitted{};
    Capacity deficit{};

    friend bool operator==(const CapacityLedger&, const CapacityLedger&) = default;
    [[nodiscard]] bool closes() const noexcept;
};

/// Everything the runtime knows about the rack at one generation.
struct SnapshotData {
    RackId rack{};
    TopologyGeneration generation{};
    RackEpoch epoch{};
    LifecycleState lifecycle = LifecycleState::kAssembling;
    Digest evidence_digest{};
    Digest member_set_digest{};
    CapacityLedger capacity{};
    std::vector<DeviceRecord> devices;        // sorted by id
    std::vector<PortRecord> ports;            // sorted by id
    std::vector<LinkRecord> links;            // sorted by id
    std::vector<AttachmentRecord> attachments;  // sorted by id
    std::vector<PathRecord> paths;            // sorted by (start, end, links)
    std::vector<MaintenanceRecord> maintenance;  // sorted by (target, source)
    std::vector<ObligationRecord> obligations;   // sorted by id
    std::vector<ResourceState> resources;     // sorted by ResourceRef
    std::vector<Diagnostic> diagnostics;      // sorted

    friend bool operator==(const SnapshotData&, const SnapshotData&) = default;

    /// Number of member devices.
    [[nodiscard]] std::size_t member_count() const noexcept { return devices.size(); }
    /// Number of eligible resources.
    [[nodiscard]] std::size_t eligible_resource_count() const noexcept;
};

[[nodiscard]] Status encode(const SnapshotData& data, ByteWriter& writer);
[[nodiscard]] Result<SnapshotData> decode_snapshot(ByteReader& reader);
[[nodiscard]] Digest snapshot_digest(const SnapshotData& data);
[[nodiscard]] Digest member_set_digest(const std::vector<DeviceRecord>& devices);

/// Immutable handle to a composed snapshot.
///
/// Threading: fully thread safe. The referenced SnapshotData is const and never
/// mutated after construction; copying a Snapshot copies a shared pointer.
class Snapshot {
public:
    Snapshot() = default;
    Snapshot(std::shared_ptr<const SnapshotData> data, Digest digest)
        : data_(std::move(data)), digest_(digest) {}

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(data_); }
    [[nodiscard]] const Digest& digest() const noexcept { return digest_; }
    [[nodiscard]] const SnapshotData& data() const;
    [[nodiscard]] const SnapshotData* operator->() const { return &data(); }

    [[nodiscard]] TopologyGeneration generation() const { return data().generation; }
    [[nodiscard]] RackEpoch epoch() const { return data().epoch; }
    [[nodiscard]] LifecycleState lifecycle() const { return data().lifecycle; }

    /// Look up the authoritative state of one resource. Returns nullptr when
    /// the resource is not part of this snapshot at all.
    [[nodiscard]] const ResourceState* find(ResourceRef ref) const;
    [[nodiscard]] const DeviceRecord* find_device(DeviceId id) const;
    [[nodiscard]] const PortRecord* find_port(PortId id) const;
    [[nodiscard]] const LinkRecord* find_link(LinkId id) const;
    [[nodiscard]] const PathRecord* find_path(PathId id) const;
    [[nodiscard]] const AttachmentRecord* find_attachment(AttachmentId id) const;

    /// Canonical encoding of the referenced snapshot data.
    [[nodiscard]] Result<std::vector<std::uint8_t>> encode() const;

private:
    std::shared_ptr<const SnapshotData> data_;
    Digest digest_{};
};

}  // namespace rnf

#endif  // RNF_COMPOSE_SNAPSHOT_HPP
