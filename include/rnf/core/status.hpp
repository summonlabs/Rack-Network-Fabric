// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Typed outcomes for every fallible operation in the runtime.
//
// The status vocabulary deliberately keeps the following conditions distinct,
// because collapsing them would let a caller mistake "I could not tell" for
// "the answer is no" or, worse, for "the answer is yes":
//
//   Unknown         - the runtime has no information at all.
//   Unsupported     - the runtime knows the concept but does not implement it.
//   Stale*          - the information existed but no longer applies.
//   Conflict        - two accepted inputs disagree and neither dominates.
//   Incomplete      - some required evidence is absent.
//   Indeterminate   - the outcome depends on state that may or may not have
//                     been durably committed (crash ambiguity).
//   Refused         - a policy or authority decision said no.
//   Cancelled       - the caller or shutdown withdrew the request.
//   Invalid*        - the input was malformed, out of range, or inconsistent.

#ifndef RNF_CORE_STATUS_HPP
#define RNF_CORE_STATUS_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace rnf {

/// Fine grained outcome code. Never widened implicitly to a boolean.
enum class StatusCode : std::uint16_t {
    kOk = 0,

    // -- caller error -------------------------------------------------------
    kInvalidArgument = 1,
    kOutOfRange,
    kMalformedEncoding,
    kInvalidUnicode,
    kOversizeField,
    kInvalidIdentity,
    kInvalidLifecycleTransition,
    kInvalidScope,

    // -- domain refusals ----------------------------------------------------
    kNotFound,
    kDuplicateIdentity,
    kConflict,
    kOutOfRack,
    kUnauthorized,
    kRefused,
    kScopeNotEligible,
    kExclusiveConflict,
    kCapacityExhausted,
    kLifecycleRefused,
    kNotAMember,
    kUnsupported,

    // -- staleness / fencing ------------------------------------------------
    kStaleGeneration,
    kStaleEpoch,
    kStaleProvenance,
    kFencedIncarnation,
    kExpired,
    kSuperseded,
    kReplayed,

    // -- incompleteness -----------------------------------------------------
    kUnknown,
    kIncomplete,
    kIndeterminate,

    // -- environment / infrastructure ---------------------------------------
    kCancelled,
    kIo,
    kCorrupt,
    kTruncated,
    kIntegrityFailure,
    kIncompatibleVersion,
    kResourceExhausted,
    kBusy,
    kClosed,
    kInternal,
};

/// Coarse grouping of StatusCode, used to keep the proof vocabulary explicit.
enum class StatusClass : std::uint8_t {
    kOk = 0,
    kInvalid,
    kRefused,
    kStale,
    kConflict,
    kIncomplete,
    kIndeterminate,
    kUnknown,
    kUnsupported,
    kCancelled,
    kEnvironment,
    kInternal,
};

/// Map a StatusCode to its class. Total function; no default-success fallback.
[[nodiscard]] constexpr StatusClass classify(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::kOk:
            return StatusClass::kOk;
        case StatusCode::kInvalidArgument:
        case StatusCode::kOutOfRange:
        case StatusCode::kMalformedEncoding:
        case StatusCode::kInvalidUnicode:
        case StatusCode::kOversizeField:
        case StatusCode::kInvalidIdentity:
        case StatusCode::kInvalidLifecycleTransition:
        case StatusCode::kInvalidScope:
        case StatusCode::kDuplicateIdentity:
            return StatusClass::kInvalid;
        case StatusCode::kNotFound:
        case StatusCode::kUnauthorized:
        case StatusCode::kRefused:
        case StatusCode::kScopeNotEligible:
        case StatusCode::kCapacityExhausted:
        case StatusCode::kLifecycleRefused:
        case StatusCode::kNotAMember:
        case StatusCode::kOutOfRack:
            return StatusClass::kRefused;
        case StatusCode::kStaleGeneration:
        case StatusCode::kStaleEpoch:
        case StatusCode::kStaleProvenance:
        case StatusCode::kFencedIncarnation:
        case StatusCode::kExpired:
        case StatusCode::kSuperseded:
        case StatusCode::kReplayed:
            return StatusClass::kStale;
        case StatusCode::kConflict:
        case StatusCode::kExclusiveConflict:
            return StatusClass::kConflict;
        case StatusCode::kIncomplete:
            return StatusClass::kIncomplete;
        case StatusCode::kIndeterminate:
            return StatusClass::kIndeterminate;
        case StatusCode::kUnknown:
            return StatusClass::kUnknown;
        case StatusCode::kUnsupported:
            return StatusClass::kUnsupported;
        case StatusCode::kCancelled:
        case StatusCode::kClosed:
            return StatusClass::kCancelled;
        case StatusCode::kIo:
        case StatusCode::kCorrupt:
        case StatusCode::kTruncated:
        case StatusCode::kIntegrityFailure:
        case StatusCode::kIncompatibleVersion:
        case StatusCode::kResourceExhausted:
        case StatusCode::kBusy:
            return StatusClass::kEnvironment;
        case StatusCode::kInternal:
            return StatusClass::kInternal;
    }
    return StatusClass::kUnknown;
}

/// Stable, lower-case, machine parseable spelling of a status code.
[[nodiscard]] constexpr std::string_view to_string(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::kOk: return "ok";
        case StatusCode::kInvalidArgument: return "invalid_argument";
        case StatusCode::kOutOfRange: return "out_of_range";
        case StatusCode::kMalformedEncoding: return "malformed_encoding";
        case StatusCode::kInvalidUnicode: return "invalid_unicode";
        case StatusCode::kOversizeField: return "oversize_field";
        case StatusCode::kInvalidIdentity: return "invalid_identity";
        case StatusCode::kInvalidLifecycleTransition: return "invalid_lifecycle_transition";
        case StatusCode::kInvalidScope: return "invalid_scope";
        case StatusCode::kNotFound: return "not_found";
        case StatusCode::kDuplicateIdentity: return "duplicate_identity";
        case StatusCode::kConflict: return "conflict";
        case StatusCode::kOutOfRack: return "out_of_rack";
        case StatusCode::kUnauthorized: return "unauthorized";
        case StatusCode::kRefused: return "refused";
        case StatusCode::kScopeNotEligible: return "scope_not_eligible";
        case StatusCode::kExclusiveConflict: return "exclusive_conflict";
        case StatusCode::kCapacityExhausted: return "capacity_exhausted";
        case StatusCode::kLifecycleRefused: return "lifecycle_refused";
        case StatusCode::kNotAMember: return "not_a_member";
        case StatusCode::kUnsupported: return "unsupported";
        case StatusCode::kStaleGeneration: return "stale_generation";
        case StatusCode::kStaleEpoch: return "stale_epoch";
        case StatusCode::kStaleProvenance: return "stale_provenance";
        case StatusCode::kFencedIncarnation: return "fenced_incarnation";
        case StatusCode::kExpired: return "expired";
        case StatusCode::kSuperseded: return "superseded";
        case StatusCode::kReplayed: return "replayed";
        case StatusCode::kUnknown: return "unknown";
        case StatusCode::kIncomplete: return "incomplete";
        case StatusCode::kIndeterminate: return "indeterminate";
        case StatusCode::kCancelled: return "cancelled";
        case StatusCode::kIo: return "io";
        case StatusCode::kCorrupt: return "corrupt";
        case StatusCode::kTruncated: return "truncated";
        case StatusCode::kIntegrityFailure: return "integrity_failure";
        case StatusCode::kIncompatibleVersion: return "incompatible_version";
        case StatusCode::kResourceExhausted: return "resource_exhausted";
        case StatusCode::kBusy: return "busy";
        case StatusCode::kClosed: return "closed";
        case StatusCode::kInternal: return "internal";
    }
    return "unknown";
}

/// Stable spelling of a status class.
[[nodiscard]] constexpr std::string_view to_string(StatusClass cls) noexcept {
    switch (cls) {
        case StatusClass::kOk: return "OK";
        case StatusClass::kInvalid: return "INVALID";
        case StatusClass::kRefused: return "REFUSED";
        case StatusClass::kStale: return "STALE";
        case StatusClass::kConflict: return "CONFLICTING";
        case StatusClass::kIncomplete: return "INCOMPLETE";
        case StatusClass::kIndeterminate: return "INDETERMINATE";
        case StatusClass::kUnknown: return "UNKNOWN";
        case StatusClass::kUnsupported: return "UNSUPPORTED";
        case StatusClass::kCancelled: return "CANCELLED";
        case StatusClass::kEnvironment: return "ENVIRONMENT";
        case StatusClass::kInternal: return "INTERNAL";
    }
    return "UNKNOWN";
}

/// A status code plus optional human readable context.
///
/// The context string is for operators. Programmatic decisions must branch on
/// the code, never on the text.
class Status {
public:
    Status() noexcept = default;
    explicit Status(StatusCode code) noexcept : code_(code) {}
    Status(StatusCode code, std::string detail) : code_(code), detail_(std::move(detail)) {}

    [[nodiscard]] StatusCode code() const noexcept { return code_; }
    [[nodiscard]] StatusClass cls() const noexcept { return classify(code_); }
    [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::kOk; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

    /// "code: detail" when a detail exists, otherwise just "code".
    [[nodiscard]] std::string describe() const;

private:
    StatusCode code_ = StatusCode::kOk;
    std::string detail_;
};

/// Outcome carrying either a value or a Status. Never throws for domain errors.
template <class T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
    Result(Status status) : storage_(std::move(status)) {}   // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] T& operator*() & { return value(); }
    [[nodiscard]] const T& operator*() const& { return value(); }
    [[nodiscard]] T* operator->() { return &value(); }
    [[nodiscard]] const T* operator->() const { return &value(); }

    [[nodiscard]] Status& error() & { return std::get<1>(storage_); }
    [[nodiscard]] const Status& error() const& { return std::get<1>(storage_); }

    [[nodiscard]] StatusCode code() const noexcept {
        return has_value() ? StatusCode::kOk : std::get<1>(storage_).code();
    }

private:
    std::variant<T, Status> storage_;
};

/// Result<void> equivalent.
template <>
class Result<void> {
public:
    Result() = default;
    Result(Status status) : status_(std::move(status)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return status_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] Status& error() noexcept { return status_; }
    [[nodiscard]] const Status& error() const noexcept { return status_; }
    [[nodiscard]] StatusCode code() const noexcept { return status_.code(); }

private:
    Status status_;
};

/// Unwrap a Result, returning early from the enclosing function on failure.
#define RNF_TRY(dest, expr)                        \
    auto dest##_rnf_result = (expr);               \
    if (!dest##_rnf_result.has_value()) {          \
        return dest##_rnf_result.error();          \
    }                                              \
    auto dest = std::move(*dest##_rnf_result)

/// Unwrap a Result<void>, returning early from the enclosing function.
#define RNF_TRYV(expr)                             \
    do {                                           \
        ::rnf::Status dest##_rnf_status = (expr);  \
        if (!dest##_rnf_status.ok()) {             \
            return dest##_rnf_status;              \
        }                                          \
    } while (false)

}  // namespace rnf

#endif  // RNF_CORE_STATUS_HPP
