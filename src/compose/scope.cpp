// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/compose/composer.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rnf {
namespace {

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

enum class Applicability : std::uint8_t { kApply, kStale, kFuture };

/// Decide whether a record says anything about the requested generation.
///
/// An ephemeral observation speaks only about the generation it names. A sticky
/// observation carries forward to every later generation but never backwards:
/// evidence gathered against generation 9 says nothing about generation 4.
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

/// Deterministic selection of the newest applicable observation for a subject.
///
/// The winner is the record with the highest (generation, sequence). Records
/// that tie on that rank but disagree on content are reported as conflicting and
/// the lexicographically smaller content digest wins, so the outcome depends
/// only on the set of records, never on their arrival order.
template <class Value>
struct Selection {
    bool present = false;
    bool conflicted = false;
    TopologyGeneration generation{};
    Sequence sequence = 0;
    Digest digest{};
    Value value{};
    SourceId source{};
};

template <class Value>
bool better_rank(const Selection<Value>& candidate, const Selection<Value>& incumbent) {
    if (candidate.generation.value != incumbent.generation.value) {
        return candidate.generation.value > incumbent.generation.value;
    }
    return candidate.sequence > incumbent.sequence;
}

/// A collector that tracks diagnostics with a hard bound.
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

    /// Sort and de-duplicate so the emitted list is canonical.
    std::vector<Diagnostic> finish() {
        std::sort(items_.begin(), items_.end());
        items_.erase(std::unique(items_.begin(), items_.end()), items_.end());
        if (dropped_ > 0) {
            Diagnostic diagnostic;
            diagnostic.kind = DiagnosticKind::kIncompleteTopology;
            diagnostic.subject_kind = EvidenceKind::kMember;
            diagnostic.detail =
                "diagnostic list was truncated at its configured bound (" +
                std::to_string(dropped_) + " entries dropped)";
            items_.push_back(std::move(diagnostic));
            std::sort(items_.begin(), items_.end());
        }
        return std::move(items_);
    }

private:
    std::size_t limit_;
    std::size_t dropped_ = 0;
    std::vector<Diagnostic> items_;
};

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
    if (value == 0) {
        value = 1;
    }
    return PathId(value);
}

[[nodiscard]] bool is_usable(Availability availability) noexcept {
    return availability == Availability::kUp || availability == Availability::kDegraded;
}

[[nodiscard]] Capacity min_capacity(Capacity a, Capacity b) noexcept {
    return a.units <= b.units ? a : b;
}

/// Reason text for an ineligible resource, kept short and stable.
[[nodiscard]] std::string ineligible_reason(const char* reason) {
    return std::string(reason);
}

}  // namespace

// ---------------------------------------------------------------------------
// Scope expansion
// ---------------------------------------------------------------------------

Result<std::vector<ResourceRef>> expand_scope(const Snapshot& snapshot, ResourceRef root) {
    if (!snapshot.valid()) {
        return Status(StatusCode::kInvalidArgument, "snapshot handle is empty");
    }
    std::vector<ResourceRef> out;
    const SnapshotData& data = snapshot.data();

    switch (root.kind) {
        case ResourceKind::kRack: {
            if (root.id != data.rack.value()) {
                return Status(StatusCode::kOutOfRack, "scope names a different rack");
            }
            out.reserve(data.resources.size());
            for (const ResourceState& state : data.resources) {
                out.push_back(state.ref);
            }
            break;
        }
        case ResourceKind::kDevice: {
            const DeviceId device(root.id);
            if (snapshot.find_device(device) == nullptr) {
                return Status(StatusCode::kNotAMember, "scope names a device that is not a member");
            }
            out.push_back(root);
            for (const PortRecord& port : data.ports) {
                if (port.device == device) {
                    out.push_back(ResourceRef{ResourceKind::kPort, port.id.value()});
                }
            }
            for (const LinkRecord& link : data.links) {
                const PortRecord* a = snapshot.find_port(link.port_a);
                const PortRecord* b = snapshot.find_port(link.port_b);
                if ((a != nullptr && a->device == device) || (b != nullptr && b->device == device)) {
                    out.push_back(ResourceRef{ResourceKind::kLink, link.id.value()});
                }
            }
            for (const AttachmentRecord& attachment : data.attachments) {
                if (attachment.device == device) {
                    out.push_back(
                        ResourceRef{ResourceKind::kAttachment, attachment.id.value()});
                }
            }
            break;
        }
        case ResourceKind::kPort: {
            const PortId port(root.id);
            if (snapshot.find_port(port) == nullptr) {
                return Status(StatusCode::kNotAMember, "scope names a port that is not in the rack");
            }
            out.push_back(root);
            for (const LinkRecord& link : data.links) {
                if (link.port_a == port || link.port_b == port) {
                    out.push_back(ResourceRef{ResourceKind::kLink, link.id.value()});
                }
            }
            for (const AttachmentRecord& attachment : data.attachments) {
                if (attachment.port == port) {
                    out.push_back(
                        ResourceRef{ResourceKind::kAttachment, attachment.id.value()});
                }
            }
            break;
        }
        case ResourceKind::kLink: {
            const LinkId link(root.id);
            const LinkRecord* record = snapshot.find_link(link);
            if (record == nullptr) {
                return Status(StatusCode::kNotAMember, "scope names a link that is not in the rack");
            }
            out.push_back(root);
            out.push_back(ResourceRef{ResourceKind::kPort, record->port_a.value()});
            out.push_back(ResourceRef{ResourceKind::kPort, record->port_b.value()});
            break;
        }
        case ResourceKind::kAttachment: {
            if (snapshot.find_attachment(AttachmentId(root.id)) == nullptr) {
                return Status(StatusCode::kNotAMember,
                              "scope names an attachment that is not in the rack");
            }
            out.push_back(root);
            break;
        }
        case ResourceKind::kPath: {
            const PathRecord* path = snapshot.find_path(PathId(root.id));
            if (path == nullptr) {
                return Status(StatusCode::kNotAMember, "scope names a path that is not in the rack");
            }
            out.push_back(root);
            for (LinkId link : path->links) {
                out.push_back(ResourceRef{ResourceKind::kLink, link.value()});
            }
            break;
        }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool scopes_intersect(const std::vector<ResourceRef>& a, const std::vector<ResourceRef>& b) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            return true;
        }
        if (a[i] < b[j]) {
            ++i;
        } else {
            ++j;
        }
    }
    return false;
}

}  // namespace rnf
