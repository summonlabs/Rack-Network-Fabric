// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The composer is a pure function from accepted evidence to an authoritative
// snapshot. It performs no input or output, holds no locks and keeps no state,
// which is what makes "equivalent evidence yields equivalent canonical state"
// a directly testable claim.

#ifndef RNF_COMPOSE_COMPOSER_HPP
#define RNF_COMPOSE_COMPOSER_HPP

#include "rnf/compose/snapshot.hpp"
#include "rnf/model/evidence.hpp"

namespace rnf {

/// Static description of the rack this runtime is authoritative for.
struct RackConfig {
    RackId rack{};
    std::string name;
    /// Capacity held in reserve and never handed out.
    Capacity headroom_floor{};
    /// Maximum number of simple paths enumerated between two edge ports.
    std::size_t max_paths_per_pair = 2;
    /// Maximum number of links in an enumerated path.
    std::size_t max_path_hops = 6;
    /// Derive ephemeral evidence only from this source when true; used by tests.
    bool strict_generation = true;

    friend bool operator==(const RackConfig&, const RackConfig&) = default;
};

/// Inputs to one composition. Named ComposeInput so that it cannot be confused
/// with the wire level ComposeCommand.
struct ComposeInput {
    TopologyGeneration generation{};
    RackEpoch epoch{};
    LifecycleState lifecycle = LifecycleState::kAssembling;
    ComposeLimits limits{};
};

/// Statistics about one composition, useful for tests and for the CLI.
struct ComposeStats {
    std::size_t slots_considered = 0;
    std::size_t slots_applied = 0;
    std::size_t slots_stale = 0;
    std::size_t slots_out_of_rack = 0;
    std::size_t member_devices = 0;
    std::size_t paths_enumerated = 0;
    std::size_t diagnostics = 0;
};

struct ComposeOutcome {
    Snapshot snapshot;
    ComposeStats stats;
};

/// Compose a snapshot. Pure: the same (ledger, config, request) always yields
/// the same digest.
[[nodiscard]] Result<ComposeOutcome> compose(const EvidenceLedger& ledger, const RackConfig& config,
                                             const ComposeInput& request);

/// Resolve the resources a scope names, given a snapshot.
///
/// Rack scope expands to every resource; device scope to the device, its ports
/// and the links that terminate on it; port scope to the port and its links;
/// link scope to the link and both of its endpoints; path scope to the path and
/// every link it traverses.
[[nodiscard]] Result<std::vector<ResourceRef>> expand_scope(const Snapshot& snapshot,
                                                            ResourceRef root);

/// True when two resource sets share at least one resource.
[[nodiscard]] bool scopes_intersect(const std::vector<ResourceRef>& a,
                                    const std::vector<ResourceRef>& b);

}  // namespace rnf

#endif  // RNF_COMPOSE_COMPOSER_HPP
