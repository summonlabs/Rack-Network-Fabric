// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Rack Network Fabric - build and protocol version constants.

#ifndef RNF_VERSION_HPP
#define RNF_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace rnf {

/// Semantic version of the Rack Network Fabric runtime.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

/// Combined numeric version, encoded as (major << 16) | (minor << 8) | patch.
inline constexpr std::uint32_t kVersionNumber =
    (kVersionMajor << 16) | (kVersionMinor << 8) | kVersionPatch;

/// Human readable version string.
inline constexpr std::string_view kVersionString = "1.0.0";

/// Wire protocol version spoken by rnfd and the client library.
///
/// The value is independent of the library version: it only changes when the
/// framing or message encoding changes in a way that is not backward
/// compatible. Peers with different protocol versions refuse the session with
/// StatusCode::kIncompatibleVersion rather than guessing.
inline constexpr std::uint16_t kProtocolVersion = 1;

/// Persistent state format version. Bumped whenever the canonical encoding of
/// persisted state changes. A store written by a different format version is
/// refused at open time (never silently reinterpreted).
inline constexpr std::uint32_t kStateFormatVersion = 1;

}  // namespace rnf

#endif  // RNF_VERSION_HPP
