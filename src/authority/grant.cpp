// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/authority/grant.hpp"

#include <algorithm>
#include <string>

#include "rnf/compose/composer.hpp"

namespace rnf {
namespace {

constexpr std::size_t kMaxPoolRefs = 64;
constexpr std::size_t kMaxReasonText = 128;

}  // namespace

std::string_view to_string(GrantMode mode) noexcept {
    switch (mode) {
        case GrantMode::kShared: return "shared";
        case GrantMode::kExclusive: return "exclusive";
    }
    return "unknown";
}

std::string_view to_string(GrantState state) noexcept {
    switch (state) {
        case GrantState::kPending: return "pending";
        case GrantState::kRecovering: return "recovering";
        case GrantState::kActive: return "active";
        case GrantState::kSuspended: return "suspended";
        case GrantState::kReleased: return "released";
        case GrantState::kExpired: return "expired";
        case GrantState::kFenced: return "fenced";
    }
    return "unknown";
}

bool parse_grant_mode(std::string_view text, GrantMode& out) noexcept {
    if (text == "shared") {
        out = GrantMode::kShared;
        return true;
    }
    if (text == "exclusive") {
        out = GrantMode::kExclusive;
        return true;
    }
    return false;
}

std::string LeaseToken::to_text() const {
    std::string out = grant.to_hex();
    out.push_back('/');
    out += std::to_string(fence);
    out.push_back('/');
    out += std::to_string(incarnation.value);
    out.push_back('/');
    out += scope_basis.to_hex();
    return out;
}

LeaseToken Grant::token() const {
    LeaseToken token;
    token.grant = id;
    token.fence = fence;
    token.incarnation = incarnation;
    token.scope_basis = authority_basis;
    return token;
}

Status encode(const Grant& grant, ByteWriter& writer) {
    writer.u64(grant.id.value());
    writer.u64(grant.request.hi);
    writer.u64(grant.request.lo);
    writer.u64(grant.principal.value());
    writer.u8(static_cast<std::uint8_t>(grant.scope.kind));
    writer.u64(grant.scope.id);
    writer.u8(static_cast<std::uint8_t>(grant.mode));
    writer.u64(grant.capacity.units);
    writer.u64(grant.generation.value);
    writer.u64(grant.epoch.value);
    writer.u64(grant.incarnation.value);
    writer.u64(grant.fence);
    writer.u64(grant.issued_at_ms);
    writer.u64(grant.expires_at_ms);
    writer.u8(static_cast<std::uint8_t>(grant.state));
    writer.digest(grant.authority_basis);
    writer.u32(static_cast<std::uint32_t>(grant.capacity_pool.size()));
    for (ResourceRef ref : grant.capacity_pool) {
        writer.u8(static_cast<std::uint8_t>(ref.kind));
        writer.u64(ref.id);
    }
    writer.text(grant.reason, kMaxReasonText);
    if (!writer.ok()) {
        return Status(StatusCode::kOversizeField, "grant record exceeds its encoding bound");
    }
    return Status{};
}

Result<Grant> decode_grant(ByteReader& reader) {
    Grant grant;
    std::uint64_t id = 0;
    std::uint8_t scope_kind = 0;
    std::uint8_t mode = 0;
    std::uint8_t state = 0;
    std::uint64_t principal = 0;
    if (!reader.u64(id) || !reader.u64(grant.request.hi) || !reader.u64(grant.request.lo) ||
        !reader.u64(principal) || !reader.u8(scope_kind) ||
        !reader.u64(grant.scope.id) || !reader.u8(mode) || !reader.u64(grant.capacity.units) ||
        !reader.u64(grant.generation.value) || !reader.u64(grant.epoch.value) ||
        !reader.u64(grant.incarnation.value) || !reader.u64(grant.fence) ||
        !reader.u64(grant.issued_at_ms) || !reader.u64(grant.expires_at_ms) || !reader.u8(state) ||
        !reader.digest(grant.authority_basis)) {
        return Status(StatusCode::kTruncated, "grant record is truncated");
    }
    grant.id = GrantId(id);
    grant.principal = PrincipalId(principal);
    grant.scope.kind = static_cast<ResourceKind>(scope_kind);
    grant.mode = static_cast<GrantMode>(mode);
    grant.state = static_cast<GrantState>(state);
    std::uint32_t pool = 0;
    if (!reader.count(pool, static_cast<std::uint32_t>(kMaxPoolRefs))) {
        return Status(StatusCode::kMalformedEncoding, "grant capacity pool exceeds its bound");
    }
    grant.capacity_pool.resize(pool);
    for (ResourceRef& ref : grant.capacity_pool) {
        std::uint8_t kind = 0;
        if (!reader.u8(kind) || !reader.u64(ref.id)) {
            return Status(StatusCode::kTruncated, "grant capacity pool is truncated");
        }
        ref.kind = static_cast<ResourceKind>(kind);
    }
    if (!reader.text(kMaxReasonText, grant.reason)) {
        return Status(StatusCode::kMalformedEncoding, "grant reason is truncated or not UTF-8");
    }
    return grant;
}

Digest grant_digest(const Grant& grant) {
    ByteWriter writer(4096);
    const Status status = encode(grant, writer);
    if (!status.ok()) {
        Blake2s256 fallback;
        fallback.update("rnf.grant.unencodable");
        return fallback.final();
    }
    return Blake2s256::hash(writer.span());
}

Digest authority_basis_digest(const Snapshot& snapshot, const std::vector<ResourceRef>& scope) {
    Blake2s256 hasher;
    hasher.update("rnf.basis.v1");
    hasher.update_le64(snapshot.data().epoch.value);
    for (ResourceRef ref : scope) {
        hasher.update_le64(static_cast<std::uint64_t>(ref.kind));
        hasher.update_le64(ref.id);
        const ResourceState* state = snapshot.find(ref);
        if (state == nullptr) {
            hasher.update("absent");
            continue;
        }
        hasher.update("present");
        hasher.update_le64(state->capacity_known ? 1U : 0U);
        hasher.update_le64(state->capacity.units);
        hasher.update_le64(static_cast<std::uint64_t>(state->owners.size()));
        for (const OwnerRef& owner : state->owners) {
            hasher.update_le64(owner.device.value());
            hasher.update_le64(owner.incarnation);
        }
    }
    return hasher.final();
}

Result<std::vector<ResourceRef>> capacity_pool_refs(const Snapshot& snapshot, ResourceRef root) {
    if (!snapshot.valid()) {
        return Status(StatusCode::kInvalidArgument, "snapshot handle is empty");
    }
    std::vector<ResourceRef> pool;
    pool.push_back(ResourceRef{ResourceKind::kRack, snapshot.data().rack.value()});

    const ResourceState* state = snapshot.find(root);
    if (state == nullptr) {
        return Status(StatusCode::kNotAMember, "scope names a resource outside this rack");
    }
    pool.push_back(root);
    for (const OwnerRef& owner : state->owners) {
        pool.push_back(ResourceRef{ResourceKind::kDevice, owner.device.value()});
    }
    std::sort(pool.begin(), pool.end());
    pool.erase(std::unique(pool.begin(), pool.end()), pool.end());
    if (pool.size() > kMaxPoolRefs) {
        return Status(StatusCode::kResourceExhausted, "capacity pool exceeds its bound");
    }
    return pool;
}

}  // namespace rnf
