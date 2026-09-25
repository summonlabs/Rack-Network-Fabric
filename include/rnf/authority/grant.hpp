// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Authority: the right to use rack resources. A grant is bound to the rack
// generation, the rack epoch, the controller incarnation that issued it, its
// resource scope, and a digest of the member incarnations its scope depends on.
// Changing any of those fences the grant rather than silently extending it.

#ifndef RNF_AUTHORITY_GRANT_HPP
#define RNF_AUTHORITY_GRANT_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rnf/compose/snapshot.hpp"
#include "rnf/core/bytes.hpp"
#include "rnf/model/types.hpp"

namespace rnf {

enum class GrantMode : std::uint8_t { kShared = 1, kExclusive = 2 };

/// Durable state of a grant. The state is stored, not inferred, so that a
/// restart cannot accidentally promote a recovering grant to active.
enum class GrantState : std::uint8_t {
    kPending = 1,     // accepted, not yet armed (used during recovery)
    kRecovering = 2,  // durable from a previous incarnation; reserves, cannot authorize
    kActive = 3,      // may authorize use
    kSuspended = 4,   // temporarily cannot authorize, still reserves
    kReleased = 5,    // returned by the holder
    kExpired = 6,     // past its expiry
    kFenced = 7,      // invalidated by a generation, epoch, incarnation or member change
};

[[nodiscard]] std::string_view to_string(GrantMode mode) noexcept;
[[nodiscard]] std::string_view to_string(GrantState state) noexcept;
[[nodiscard]] bool parse_grant_mode(std::string_view text, GrantMode& out) noexcept;

/// The bearer token handed to the holder.
///
/// A token is only meaningful together with the daemon incarnation that issued
/// it: a token from a previous incarnation is refused even if every other field
/// matches, which is what fences authority across a restart.
struct LeaseToken {
    GrantId grant{};
    FencingToken fence = 0;
    ControllerIncarnation incarnation{};
    Digest scope_basis{};

    friend bool operator==(const LeaseToken&, const LeaseToken&) = default;
    [[nodiscard]] std::string to_text() const;
};

struct Grant {
    GrantId id{};
    RequestId request{};
    PrincipalId principal{};
    ResourceRef scope{};
    GrantMode mode = GrantMode::kShared;
    Capacity capacity{};
    TopologyGeneration generation{};
    RackEpoch epoch{};
    ControllerIncarnation incarnation{};
    FencingToken fence = 0;
    TimestampMs issued_at_ms = 0;
    TimestampMs expires_at_ms = 0;
    GrantState state = GrantState::kPending;
    Digest authority_basis{};
    /// The exact capacity pools this grant draws from. Stored rather than
    /// recomputed so that releasing a grant always returns precisely the
    /// capacity it took, even after the topology moved on.
    std::vector<ResourceRef> capacity_pool;
    std::string reason;

    friend bool operator==(const Grant&, const Grant&) = default;
    [[nodiscard]] LeaseToken token() const;
};

/// A request for authority. RequestId makes the request idempotent: replaying
/// the same id returns the original outcome instead of granting twice.
struct GrantRequest {
    RequestId request{};
    PrincipalId principal{};
    ResourceRef scope{};
    GrantMode mode = GrantMode::kShared;
    Capacity capacity{};
    std::uint64_t ttl_ms = 60000;
};

[[nodiscard]] Status encode(const Grant& grant, ByteWriter& writer);
[[nodiscard]] Result<Grant> decode_grant(ByteReader& reader);
[[nodiscard]] Digest grant_digest(const Grant& grant);

/// Canonical digest of the member incarnations a scope depends on.
///
/// This is the fingerprint that makes member reincarnation fence authority: a
/// device that leaves and rejoins under a new incarnation produces a different
/// basis, so every grant whose scope touched that device stops validating.
[[nodiscard]] Digest authority_basis_digest(const Snapshot& snapshot,
                                            const std::vector<ResourceRef>& scope);

/// Resources whose capacity pool a grant on this root draws from.
///
/// Always includes the rack pool, the root itself, and every device that owns
/// the root. A rack scope therefore only consumes the rack pool, while a link
/// scope consumes the link, both endpoint devices and the rack.
[[nodiscard]] Result<std::vector<ResourceRef>> capacity_pool_refs(const Snapshot& snapshot,
                                                                 ResourceRef root);

}  // namespace rnf

#endif  // RNF_AUTHORITY_GRANT_HPP
