// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/compose/snapshot.hpp"

#include <algorithm>

namespace rnf {
namespace {

const ComposeLimits kLimits{};

constexpr std::uint32_t kMaxOwnersPerResource = 64;
constexpr std::size_t kMaxDiagnosticDetail = 256;

void encode_resource(const ResourceState& state, ByteWriter& writer) {
    writer.u8(static_cast<std::uint8_t>(state.ref.kind));
    writer.u64(state.ref.id);
    writer.u8(static_cast<std::uint8_t>(state.eligibility));
    writer.u8(static_cast<std::uint8_t>(state.availability));
    writer.boolean(state.capacity_known);
    writer.u64(state.capacity.units);
    writer.u64(state.obligated.units);
    writer.boolean(state.maintenance);
    writer.u32(static_cast<std::uint32_t>(state.owners.size()));
    for (const OwnerRef& owner : state.owners) {
        writer.u64(owner.device.value());
        writer.u64(owner.incarnation);
    }
}

bool decode_resource(ByteReader& reader, ResourceState& state) {
    std::uint8_t kind = 0;
    std::uint8_t eligibility = 0;
    std::uint8_t availability = 0;
    if (!reader.u8(kind) || !reader.u64(state.ref.id) || !reader.u8(eligibility) ||
        !reader.u8(availability) || !reader.boolean(state.capacity_known) ||
        !reader.u64(state.capacity.units) || !reader.u64(state.obligated.units) ||
        !reader.boolean(state.maintenance)) {
        return false;
    }
    state.ref.kind = static_cast<ResourceKind>(kind);
    state.eligibility = static_cast<Eligibility>(eligibility);
    state.availability = static_cast<Availability>(availability);
    std::uint32_t owners = 0;
    if (!reader.count(owners, kMaxOwnersPerResource)) {
        return false;
    }
    state.owners.resize(owners);
    for (std::uint32_t i = 0; i < owners; ++i) {
        std::uint64_t device = 0;
        if (!reader.u64(device) || !reader.u64(state.owners[i].incarnation)) {
            return false;
        }
        state.owners[i].device = DeviceId(device);
    }
    return true;
}

}  // namespace

const ComposeLimits& default_compose_limits() noexcept {
    return kLimits;
}

std::size_t SnapshotData::eligible_resource_count() const noexcept {
    std::size_t count = 0;
    for (const ResourceState& state : resources) {
        if (state.eligibility == Eligibility::kEligible) {
            ++count;
        }
    }
    return count;
}

bool CapacityLedger::closes() const noexcept {
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

Digest member_set_digest(const std::vector<DeviceRecord>& devices) {
    Blake2s256 hasher;
    hasher.update("rnf.members.v1");
    for (const DeviceRecord& device : devices) {
        std::uint8_t buf[16];
        for (int i = 0; i < 8; ++i) {
            buf[i] = static_cast<std::uint8_t>((device.id.value() >> (8 * i)) & 0xFFU);
            buf[8 + i] = static_cast<std::uint8_t>((device.incarnation >> (8 * i)) & 0xFFU);
        }
        hasher.update(ByteSpan(buf, sizeof(buf)));
    }
    return hasher.final();
}

Status encode(const SnapshotData& data, ByteWriter& writer) {
    writer.u64(data.rack.value());
    writer.u64(data.generation.value);
    writer.u64(data.epoch.value);
    writer.u8(static_cast<std::uint8_t>(data.lifecycle));
    writer.digest(data.evidence_digest);
    writer.digest(data.member_set_digest);

    writer.u64(data.capacity.total.units);
    writer.u64(data.capacity.unavailable.units);
    writer.u64(data.capacity.usable.units);
    writer.u64(data.capacity.obligated.units);
    writer.u64(data.capacity.headroom_floor.units);
    writer.u64(data.capacity.uncommitted.units);
    writer.u64(data.capacity.deficit.units);

    writer.u32(static_cast<std::uint32_t>(data.devices.size()));
    for (const DeviceRecord& device : data.devices) {
        writer.u64(device.id.value());
        writer.u8(static_cast<std::uint8_t>(device.role));
        writer.u64(device.incarnation);
        writer.u64(device.capacity.units);
        writer.boolean(device.capacity_known);
        writer.u8(static_cast<std::uint8_t>(device.eligibility));
        writer.u8(static_cast<std::uint8_t>(device.availability));
    }

    writer.u32(static_cast<std::uint32_t>(data.ports.size()));
    for (const PortRecord& port : data.ports) {
        writer.u64(port.id.value());
        writer.u64(port.device.value());
        writer.u16(port.index);
        writer.u8(static_cast<std::uint8_t>(port.role));
        writer.u64(port.capacity.units);
        writer.boolean(port.capacity_known);
        writer.u8(static_cast<std::uint8_t>(port.availability));
        writer.u8(static_cast<std::uint8_t>(port.admin));
        writer.u8(static_cast<std::uint8_t>(port.eligibility));
    }

    writer.u32(static_cast<std::uint32_t>(data.links.size()));
    for (const LinkRecord& link : data.links) {
        writer.u64(link.id.value());
        writer.u64(link.port_a.value());
        writer.u64(link.port_b.value());
        writer.u8(static_cast<std::uint8_t>(link.kind));
        writer.u64(link.capacity.units);
        writer.boolean(link.capacity_known);
        writer.u8(static_cast<std::uint8_t>(link.availability));
        writer.u8(static_cast<std::uint8_t>(link.eligibility));
    }

    writer.u32(static_cast<std::uint32_t>(data.attachments.size()));
    for (const AttachmentRecord& attachment : data.attachments) {
        writer.u64(attachment.id.value());
        writer.u64(attachment.device.value());
        writer.u64(attachment.port.value());
        writer.text(attachment.host_key, kMaxTextField);
        writer.u8(static_cast<std::uint8_t>(attachment.eligibility));
    }

    writer.u32(static_cast<std::uint32_t>(data.paths.size()));
    for (const PathRecord& path : data.paths) {
        writer.u64(path.id.value());
        writer.u64(path.start_port.value());
        writer.u64(path.end_port.value());
        writer.u32(static_cast<std::uint32_t>(path.links.size()));
        for (LinkId link : path.links) {
            writer.u64(link.value());
        }
        writer.u64(path.bottleneck.units);
        writer.boolean(path.bottleneck_known);
        writer.u8(static_cast<std::uint8_t>(path.eligibility));
    }

    writer.u32(static_cast<std::uint32_t>(data.maintenance.size()));
    for (const MaintenanceRecord& record : data.maintenance) {
        writer.u8(static_cast<std::uint8_t>(record.action));
        writer.u8(static_cast<std::uint8_t>(record.target.kind));
        writer.u64(record.target.id);
        writer.u64(record.source.value());
        writer.u64(record.sequence);
        writer.text(record.reason, kMaxTextField);
    }

    writer.u32(static_cast<std::uint32_t>(data.obligations.size()));
    for (const ObligationRecord& record : data.obligations) {
        writer.u64(record.id.value());
        writer.u8(static_cast<std::uint8_t>(record.target.kind));
        writer.u64(record.target.id);
        writer.u64(record.capacity.units);
        writer.u64(record.holder.value());
        writer.u64(record.source.value());
        writer.text(record.authority, kMaxTextField);
    }

    writer.u32(static_cast<std::uint32_t>(data.resources.size()));
    for (const ResourceState& state : data.resources) {
        encode_resource(state, writer);
    }

    writer.u32(static_cast<std::uint32_t>(data.diagnostics.size()));
    for (const Diagnostic& diagnostic : data.diagnostics) {
        writer.u8(static_cast<std::uint8_t>(diagnostic.kind));
        writer.u8(static_cast<std::uint8_t>(diagnostic.subject_kind));
        writer.u64(diagnostic.subject_id);
        writer.u64(diagnostic.source.value());
        writer.text(diagnostic.detail, kMaxDiagnosticDetail);
    }

    if (!writer.ok()) {
        return Status(StatusCode::kResourceExhausted, "snapshot encoding exceeded its bound");
    }
    return Status{};
}

namespace {

Result<std::vector<std::uint8_t>> encode_snapshot_bytes(const SnapshotData& data,
                                                        std::size_t limit) {
    ByteWriter writer(limit);
    RNF_TRYV(encode(data, writer));
    return writer.take();
}

}  // namespace

Result<SnapshotData> decode_snapshot(ByteReader& reader) {
    SnapshotData data;
    std::uint64_t rack = 0;
    std::uint8_t lifecycle = 0;
    if (!reader.u64(rack) || !reader.u64(data.generation.value) || !reader.u64(data.epoch.value) ||
        !reader.u8(lifecycle) || !reader.digest(data.evidence_digest) ||
        !reader.digest(data.member_set_digest)) {
        return Status(StatusCode::kTruncated, "snapshot header is truncated");
    }
    data.rack = RackId(rack);
    data.lifecycle = static_cast<LifecycleState>(lifecycle);

    if (!reader.u64(data.capacity.total.units) || !reader.u64(data.capacity.unavailable.units) ||
        !reader.u64(data.capacity.usable.units) || !reader.u64(data.capacity.obligated.units) ||
        !reader.u64(data.capacity.headroom_floor.units) ||
        !reader.u64(data.capacity.uncommitted.units) ||
        !reader.u64(data.capacity.deficit.units)) {
        return Status(StatusCode::kTruncated, "capacity ledger is truncated");
    }

    std::uint32_t count = 0;
    if (!reader.count(count, static_cast<std::uint32_t>(kLimits.max_devices))) {
        return Status(StatusCode::kMalformedEncoding, "device count exceeds its bound");
    }
    data.devices.resize(count);
    for (DeviceRecord& device : data.devices) {
        std::uint64_t id = 0;
        std::uint8_t role = 0;
        std::uint8_t eligibility = 0;
        std::uint8_t availability = 0;
        if (!reader.u64(id) || !reader.u8(role) || !reader.u64(device.incarnation) ||
            !reader.u64(device.capacity.units) || !reader.boolean(device.capacity_known) ||
            !reader.u8(eligibility) || !reader.u8(availability)) {
            return Status(StatusCode::kTruncated, "device record is truncated");
        }
        device.id = DeviceId(id);
        device.role = static_cast<DeviceRole>(role);
        device.eligibility = static_cast<Eligibility>(eligibility);
        device.availability = static_cast<Availability>(availability);
    }

    if (!reader.count(count, static_cast<std::uint32_t>(kLimits.max_ports))) {
        return Status(StatusCode::kMalformedEncoding, "port count exceeds its bound");
    }
    data.ports.resize(count);
    for (PortRecord& port : data.ports) {
        std::uint64_t id = 0;
        std::uint64_t device = 0;
        std::uint8_t role = 0;
        std::uint8_t availability = 0;
        std::uint8_t admin = 0;
        std::uint8_t eligibility = 0;
        if (!reader.u64(id) || !reader.u64(device) || !reader.u16(port.index) ||
            !reader.u8(role) || !reader.u64(port.capacity.units) ||
            !reader.boolean(port.capacity_known) || !reader.u8(availability) ||
            !reader.u8(admin) || !reader.u8(eligibility)) {
            return Status(StatusCode::kTruncated, "port record is truncated");
        }
        port.id = PortId(id);
        port.device = DeviceId(device);
        port.role = static_cast<PortRole>(role);
        port.availability = static_cast<Availability>(availability);
        port.admin = static_cast<AdminState>(admin);
        port.eligibility = static_cast<Eligibility>(eligibility);
    }

    if (!reader.count(count, static_cast<std::uint32_t>(kLimits.max_links))) {
        return Status(StatusCode::kMalformedEncoding, "link count exceeds its bound");
    }
    data.links.resize(count);
    for (LinkRecord& link : data.links) {
        std::uint64_t id = 0;
        std::uint64_t port_a = 0;
        std::uint64_t port_b = 0;
        std::uint8_t kind = 0;
        std::uint8_t availability = 0;
        std::uint8_t eligibility = 0;
        if (!reader.u64(id) || !reader.u64(port_a) || !reader.u64(port_b) || !reader.u8(kind) ||
            !reader.u64(link.capacity.units) || !reader.boolean(link.capacity_known) ||
            !reader.u8(availability) || !reader.u8(eligibility)) {
            return Status(StatusCode::kTruncated, "link record is truncated");
        }
        link.id = LinkId(id);
        link.port_a = PortId(port_a);
        link.port_b = PortId(port_b);
        link.kind = static_cast<LinkKind>(kind);
        link.availability = static_cast<Availability>(availability);
        link.eligibility = static_cast<Eligibility>(eligibility);
    }

    if (!reader.count(count, static_cast<std::uint32_t>(kLimits.max_attachments))) {
        return Status(StatusCode::kMalformedEncoding, "attachment count exceeds its bound");
    }
    data.attachments.resize(count);
    for (AttachmentRecord& attachment : data.attachments) {
        std::uint64_t id = 0;
        std::uint64_t device = 0;
        std::uint64_t port = 0;
        std::uint8_t eligibility = 0;
        if (!reader.u64(id) || !reader.u64(device) || !reader.u64(port) ||
            !reader.text(kMaxTextField, attachment.host_key) || !reader.u8(eligibility)) {
            return Status(StatusCode::kTruncated, "attachment record is truncated");
        }
        attachment.id = AttachmentId(id);
        attachment.device = DeviceId(device);
        attachment.port = PortId(port);
        attachment.eligibility = static_cast<Eligibility>(eligibility);
    }

    if (!reader.count(count, static_cast<std::uint32_t>(kLimits.max_paths))) {
        return Status(StatusCode::kMalformedEncoding, "path count exceeds its bound");
    }
    data.paths.resize(count);
    for (PathRecord& path : data.paths) {
        std::uint64_t id = 0;
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        std::uint32_t link_count = 0;
        std::uint8_t eligibility = 0;
        if (!reader.u64(id) || !reader.u64(start) || !reader.u64(end)) {
            return Status(StatusCode::kTruncated, "path record is truncated");
        }
        path.id = PathId(id);
        path.start_port = PortId(start);
        path.end_port = PortId(end);
        if (!reader.count(link_count, static_cast<std::uint32_t>(kLimits.max_links))) {
            return Status(StatusCode::kMalformedEncoding, "path link count exceeds its bound");
        }
        path.links.resize(link_count);
        for (LinkId& link : path.links) {
            std::uint64_t value = 0;
            if (!reader.u64(value)) {
                return Status(StatusCode::kTruncated, "path link list is truncated");
            }
            link = LinkId(value);
        }
        if (!reader.u64(path.bottleneck.units) || !reader.boolean(path.bottleneck_known) ||
            !reader.u8(eligibility)) {
            return Status(StatusCode::kTruncated, "path record is truncated");
        }
        path.eligibility = static_cast<Eligibility>(eligibility);
    }

    if (!reader.count(count, static_cast<std::uint32_t>(kLimits.max_maintenance))) {
        return Status(StatusCode::kMalformedEncoding, "maintenance count exceeds its bound");
    }
    data.maintenance.resize(count);
    for (MaintenanceRecord& record : data.maintenance) {
        std::uint8_t action = 0;
        std::uint8_t target_kind = 0;
        std::uint64_t source = 0;
        if (!reader.u8(action) || !reader.u8(target_kind) || !reader.u64(record.target.id) ||
            !reader.u64(source) || !reader.u64(record.sequence) ||
            !reader.text(kMaxTextField, record.reason)) {
            return Status(StatusCode::kTruncated, "maintenance record is truncated");
        }
        record.action = static_cast<MaintenanceKind>(action);
        record.target.kind = static_cast<ResourceKind>(target_kind);
        record.source = SourceId(source);
    }

    if (!reader.count(count, static_cast<std::uint32_t>(kLimits.max_obligations))) {
        return Status(StatusCode::kMalformedEncoding, "obligation count exceeds its bound");
    }
    data.obligations.resize(count);
    for (ObligationRecord& record : data.obligations) {
        std::uint64_t id = 0;
        std::uint8_t target_kind = 0;
        std::uint64_t holder = 0;
        std::uint64_t source = 0;
        if (!reader.u64(id) || !reader.u8(target_kind) || !reader.u64(record.target.id) ||
            !reader.u64(record.capacity.units) || !reader.u64(holder) || !reader.u64(source) ||
            !reader.text(kMaxTextField, record.authority)) {
            return Status(StatusCode::kTruncated, "obligation record is truncated");
        }
        record.id = ObligationId(id);
        record.target.kind = static_cast<ResourceKind>(target_kind);
        record.holder = PrincipalId(holder);
        record.source = SourceId(source);
    }

    if (!reader.count(count, static_cast<std::uint32_t>(kLimits.max_ports + kLimits.max_links +
                                                       kLimits.max_paths + kLimits.max_devices +
                                                       4))) {
        return Status(StatusCode::kMalformedEncoding, "resource count exceeds its bound");
    }
    data.resources.resize(count);
    for (ResourceState& state : data.resources) {
        if (!decode_resource(reader, state)) {
            return Status(StatusCode::kTruncated, "resource state is truncated");
        }
    }

    if (!reader.count(count, static_cast<std::uint32_t>(kLimits.max_diagnostics))) {
        return Status(StatusCode::kMalformedEncoding, "diagnostic count exceeds its bound");
    }
    data.diagnostics.resize(count);
    for (Diagnostic& diagnostic : data.diagnostics) {
        std::uint8_t kind = 0;
        std::uint8_t subject_kind = 0;
        std::uint64_t source = 0;
        if (!reader.u8(kind) || !reader.u8(subject_kind) || !reader.u64(diagnostic.subject_id) ||
            !reader.u64(source) || !reader.text(kMaxDiagnosticDetail, diagnostic.detail)) {
            return Status(StatusCode::kTruncated, "diagnostic record is truncated");
        }
        diagnostic.kind = static_cast<DiagnosticKind>(kind);
        diagnostic.subject_kind = static_cast<EvidenceKind>(subject_kind);
        diagnostic.source = SourceId(source);
    }

    if (!reader.at_end()) {
        return Status(StatusCode::kMalformedEncoding, "trailing bytes after snapshot payload");
    }
    if (!data.capacity.closes()) {
        return Status(StatusCode::kIntegrityFailure, "decoded capacity ledger does not close");
    }
    return data;
}

Digest snapshot_digest(const SnapshotData& data) {
    ByteWriter writer(kLimits.max_encoded_snapshot);
    const Status status = encode(data, writer);
    if (!status.ok()) {
        Blake2s256 fallback;
        fallback.update("rnf.snapshot.unencodable");
        fallback.update_le64(static_cast<std::uint64_t>(status.code()));
        return fallback.final();
    }
    return Blake2s256::hash(writer.span());
}

const SnapshotData& Snapshot::data() const {
    static const SnapshotData kEmpty;
    return data_ ? *data_ : kEmpty;
}

Result<std::vector<std::uint8_t>> Snapshot::encode() const {
    if (!data_) {
        return Status(StatusCode::kInvalidArgument, "snapshot handle is empty");
    }
    return encode_snapshot_bytes(*data_, kLimits.max_encoded_snapshot);
}

namespace {

template <class Container, class Key, class Projection>
const typename Container::value_type* find_sorted(const Container& container, const Key& key,
                                                  Projection project) {
    const auto it = std::lower_bound(
        container.begin(), container.end(), key,
        [&project](const typename Container::value_type& element, const Key& value) {
            return project(element) < value;
        });
    if (it == container.end() || !(project(*it) == key)) {
        return nullptr;
    }
    return &*it;
}

}  // namespace

const ResourceState* Snapshot::find(ResourceRef ref) const {
    return find_sorted(data().resources, ref, [](const ResourceState& s) { return s.ref; });
}

const DeviceRecord* Snapshot::find_device(DeviceId id) const {
    return find_sorted(data().devices, id, [](const DeviceRecord& d) { return d.id; });
}

const PortRecord* Snapshot::find_port(PortId id) const {
    return find_sorted(data().ports, id, [](const PortRecord& p) { return p.id; });
}

const LinkRecord* Snapshot::find_link(LinkId id) const {
    return find_sorted(data().links, id, [](const LinkRecord& l) { return l.id; });
}

const PathRecord* Snapshot::find_path(PathId id) const {
    return find_sorted(data().paths, id, [](const PathRecord& p) { return p.id; });
}

const AttachmentRecord* Snapshot::find_attachment(AttachmentId id) const {
    return find_sorted(data().attachments, id, [](const AttachmentRecord& a) { return a.id; });
}

}  // namespace rnf
