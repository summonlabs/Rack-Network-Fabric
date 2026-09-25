// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// rnfctl - inspection and control client for rnfd.
//
// Output is line oriented JSON so operators can pipe it into other tools. The
// client only ever connects to the endpoint given on the command line.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "rnf/core/log.hpp"
#include "rnf/net/socket.hpp"
#include "rnf/runtime/client.hpp"
#include "rnf/version.hpp"

namespace {

constexpr char kQuote = static_cast<char>(0x22);
constexpr char kBackslash = static_cast<char>(0x5C);

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == kQuote) {
            out += kBackslash;
            out += kQuote;
        } else if (c == kBackslash) {
            out += kBackslash;
            out += kBackslash;
        } else if (u < 0x20U) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "%s%04x", "u", static_cast<unsigned>(u));
            out += kBackslash;
            out += buffer;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// A JSON string literal that is already quoted, so the field helpers can tell
/// "quote this for me" apart from "this is already a JSON value".
struct Quoted {
    std::string text;
};

Quoted quote(std::string_view text) {
    Quoted out;
    out.text.push_back(kQuote);
    out.text += json_escape(text);
    out.text.push_back(kQuote);
    return out;
}

std::string field(std::string_view key, const Quoted& value) {
    return quote(key).text + ":" + value.text;
}

std::string field(std::string_view key, const std::string& value) {
    return quote(key).text + ":" + quote(value).text;
}

std::string field(std::string_view key, std::uint64_t value) {
    return quote(key).text + ":" + std::to_string(value);
}

std::string boolean_field(std::string_view key, bool value) {
    return quote(key).text + ":" + (value ? "true" : "false");
}

void print_usage() {
    std::cout <<
        "rnfctl " << rnf::kVersionString << " - rack fabric client\n"
        "\n"
        "Usage: rnfctl --endpoint <host:port> <command> [options]\n"
        "\n"
        "Inspection:\n"
        "  hello | state | stats | paths | grants\n"
        "  resources [--limit N] [--kind K]\n"
        "\n"
        "Control:\n"
        "  compose --generation N | lifecycle <state> | checkpoint | shutdown\n"
        "\n"
        "Evidence (each accepts --source N --sequence N --generation N --durability S):\n"
        "  declare-member --device N [--member-rack N] [--incarnation N]\n"
        "  declare-device --device N [--role R] [--capacity N] [--availability A]\n"
        "  declare-port --port N --device N --index N [--port-role R] [--capacity N]\n"
        "               [--admin A] [--availability A]\n"
        "  declare-link --link N --port-a N --port-b N [--kind K] [--capacity N]\n"
        "               [--availability A]\n"
        "  attach --attachment N --device N --port N [--host-key S]\n"
        "  maintenance --action drain|maintenance|clear --target <kind>:<id> [--reason S]\n"
        "  obligate --obligation N --target <kind>:<id> --capacity N --holder N [--authority S]\n"
        "\n"
        "Authority:\n"
        "  acquire --request <32 hex> --principal N --scope <kind>:<id>\n"
        "          [--mode shared|exclusive] [--capacity N] [--ttl-ms N]\n"
        "  renew | release | validate with --grant N --fence N --incarnation N --basis <64 hex>\n"
        "  lookup --request <32 hex>\n"
        "\n"
        "Common options: --endpoint <host:port>, --timeout-ms N, --quiet, --log-level L\n";
}

struct Arguments {
    std::string command;
    std::map<std::string, std::string> values;
    std::string endpoint = "127.0.0.1:8790";
    std::uint32_t timeout_ms = 300000;
    bool quiet = false;
    bool valid = true;
    std::string error;

    [[nodiscard]] std::string get(std::string_view key, std::string fallback = {}) const {
        const auto it = values.find(std::string(key));
        return it == values.end() ? fallback : it->second;
    }
    [[nodiscard]] bool has(std::string_view key) const {
        return values.find(std::string(key)) != values.end();
    }
};

bool parse_u64(std::string_view text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    out = value;
    return true;
}

Arguments parse_arguments(int argc, char** argv) {
    Arguments arguments;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help" || flag == "-h") {
            print_usage();
            std::exit(0);
        }
        if (flag == "--quiet") {
            arguments.quiet = true;
            continue;
        }
        if (flag.rfind("--", 0) != 0) {
            if (arguments.command.empty()) {
                arguments.command = flag;
                continue;
            }
            arguments.valid = false;
            arguments.error = "unexpected argument " + flag;
            return arguments;
        }
        if (i + 1 >= argc) {
            arguments.valid = false;
            arguments.error = flag + " requires a value";
            return arguments;
        }
        const std::string value = argv[++i];
        if (flag == "--endpoint") {
            arguments.endpoint = value;
        } else if (flag == "--timeout-ms") {
            std::uint64_t parsed = 0;
            if (!parse_u64(value, parsed) || parsed > 0xFFFFFFFFULL) {
                arguments.valid = false;
                arguments.error = "--timeout-ms needs a number";
                return arguments;
            }
            arguments.timeout_ms = static_cast<std::uint32_t>(parsed);
        } else if (flag == "--log-level") {
            rnf::LogLevel level = rnf::LogLevel::kInfo;
            if (!rnf::parse_log_level(value, level)) {
                arguments.valid = false;
                arguments.error = "unknown log level";
                return arguments;
            }
            rnf::Logger::instance().set_level(level);
        } else {
            arguments.values[flag.substr(2)] = value;
        }
    }
    if (arguments.command.empty()) {
        arguments.valid = false;
        arguments.error = "no command given";
    }
    return arguments;
}

rnf::Result<rnf::Endpoint> parse_endpoint(const std::string& text) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return rnf::Status(rnf::StatusCode::kInvalidArgument,
                           "endpoint must be written host:port");
    }
    rnf::Endpoint endpoint;
    endpoint.host = text.substr(0, colon);
    std::uint64_t port = 0;
    if (!parse_u64(text.substr(colon + 1), port) || port == 0 || port > 65535) {
        return rnf::Status(rnf::StatusCode::kOutOfRange, "endpoint port is out of range");
    }
    endpoint.port = static_cast<std::uint16_t>(port);
    return endpoint;
}

std::uint64_t default_sequence() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void emit(const Arguments& arguments, const std::string& result, const std::string& body) {
    if (arguments.quiet) {
        return;
    }
    std::cout << "{" << field("result", quote(result)) << (body.empty() ? "" : ",") << body
              << "}\n";
}

void emit_status(const Arguments& arguments, const std::string& result, const rnf::Status& status) {
    if (arguments.quiet) {
        return;
    }
    std::cout << "{" << field("result", quote(result)) << ","
              << field("class", std::string(rnf::to_string(status.cls()))) << ","
              << field("code", std::string(rnf::to_string(status.code()))) << ","
              << field("detail", status.detail()) << "}\n";
    std::cerr << "rnfctl: " << status.describe() << "\n";
}

/// Unwrap a Result inside a command handler, reporting the failure and
/// returning the CLI exit code 1 when the result is not a value.
#define CTL_TRY(dest, expr)                                     \
    auto dest##_ctl_result = (expr);                            \
    if (!dest##_ctl_result.has_value()) {                       \
        emit_status(arguments, command, dest##_ctl_result.error()); \
        return 1;                                               \
    }                                                           \
    auto& dest = *dest##_ctl_result




/// Common evidence fields shared by every declare command.
struct EvidenceDefaults {
    rnf::SourceId source{1};
    std::uint64_t sequence = 0;
    rnf::TopologyGeneration generation{};
    rnf::Durability durability = rnf::Durability::kSticky;
    bool ok = true;
    std::string error;
};

EvidenceDefaults evidence_defaults(const Arguments& arguments,
                                   const rnf::StateResponse& state) {
    EvidenceDefaults defaults;
    defaults.sequence = default_sequence();
    defaults.generation = state.generation;
    std::uint64_t number = 0;
    if (arguments.has("source")) {
        if (!parse_u64(arguments.get("source"), number) || number == 0) {
            defaults.ok = false;
            defaults.error = "--source needs a positive identity";
            return defaults;
        }
        defaults.source = rnf::SourceId(number);
    }
    if (arguments.has("sequence")) {
        if (!parse_u64(arguments.get("sequence"), number)) {
            defaults.ok = false;
            defaults.error = "--sequence needs a number";
            return defaults;
        }
        defaults.sequence = number;
    }
    if (arguments.has("generation")) {
        if (!parse_u64(arguments.get("generation"), number)) {
            defaults.ok = false;
            defaults.error = "--generation needs a number";
            return defaults;
        }
        defaults.generation = rnf::TopologyGeneration{number};
    }
    if (arguments.has("durability")) {
        if (!rnf::parse_durability(arguments.get("durability"), defaults.durability)) {
            defaults.ok = false;
            defaults.error = "--durability must be sticky or ephemeral";
            return defaults;
        }
    }
    return defaults;
}

rnf::Result<std::uint64_t> required_number(const Arguments& arguments, std::string_view key) {
    const std::string text = arguments.get(key);
    if (text.empty()) {
        return rnf::Status(rnf::StatusCode::kInvalidArgument,
                           "--" + std::string(key) + " is required");
    }
    std::uint64_t value = 0;
    if (!parse_u64(text, value)) {
        return rnf::Status(rnf::StatusCode::kInvalidArgument,
                           "--" + std::string(key) + " needs a number");
    }
    return value;
}

rnf::Result<rnf::SourceKind> parse_source_kind(const Arguments& arguments) {
    const std::string text = arguments.get("source-kind", "operator");
    rnf::SourceKind kind = rnf::SourceKind::kOperator;
    if (!rnf::parse_source_kind(text, kind)) {
        return rnf::Status(rnf::StatusCode::kInvalidArgument,
                           "--source-kind must be operator, discovery, imported or synthetic");
    }
    return kind;
}

int run_command(const Arguments& arguments, rnf::Client& client, const rnf::StateResponse& state) {
    const std::string& command = arguments.command;

    // -- inspection ---------------------------------------------------------
    if (command == "state") {
        CTL_TRY(state_view, client.query_state());
        emit(arguments, "state",
             field("rack", state_view.rack.to_hex()) + "," +
                 field("generation", state_view.generation.value) + "," +
                 field("epoch", state_view.epoch.value) + "," +
                 field("incarnation", state_view.incarnation.value) + "," +
                 field("lifecycle", std::string(rnf::to_string(state_view.lifecycle))) + "," +
                 field("snapshot_digest", quote(state_view.snapshot_digest.to_hex())) + "," +
                 field("evidence_digest", quote(state_view.evidence_digest.to_hex())) + "," +
                 field("total_capacity", state_view.capacity.total.units) + "," +
                 field("unavailable_capacity", state_view.capacity.unavailable.units) + "," +
                 field("usable_capacity", state_view.capacity.usable.units) + "," +
                 field("obligated_capacity", state_view.capacity.obligated.units) + "," +
                 field("committed_capacity", state_view.capacity.committed_grants.units) + "," +
                 field("headroom_floor", state_view.capacity.headroom_floor.units) + "," +
                 field("uncommitted_capacity", state_view.capacity.uncommitted.units) + "," +
                 field("capacity_deficit", state_view.capacity.deficit.units) + "," +
                 field("devices", state_view.devices) + "," + field("ports", state_view.ports) +
                 "," + field("links", state_view.links) + "," +
                 field("attachments", state_view.attachments) + "," +
                 field("paths", state_view.paths) + "," +
                 field("live_grants", state_view.live_grants) + "," +
                 field("recovering_grants", state_view.recovering_grants) + "," +
                 field("total_diagnostics", state_view.total_diagnostics) + "," +
                 field("dirty_recovery", state_view.recovered_from_dirty_shutdown ? 1 : 0));
        return 0;
    }
    if (command == "stats") {
        CTL_TRY(view, client.stats());
        emit(arguments, "stats",
             field("uptime_ms", view.uptime_ms) + "," +
                 field("evidence_records", view.evidence_records) + "," +
                 field("evidence_sources", view.evidence_sources) + "," +
                 field("evidence_conflicts", view.evidence_conflicts) + "," +
                 field("evidence_superseded", view.evidence_superseded) + "," +
                 field("log_records", view.log_records) + "," +
                 field("log_bytes", view.log_bytes) + "," +
                 field("checkpoints", view.checkpoints) + "," +
                 field("requests_served", view.requests_served) + "," +
                 field("connections_accepted", view.connections_accepted) + "," +
                 field("connections_rejected", view.connections_rejected) + "," +
                 field("frames_rejected", view.frames_rejected) + "," +
                 field("bytes_sent", view.bytes_sent) + "," +
                 field("bytes_received", view.bytes_received) + "," +
                 field("composes", view.composes) + "," +
                 field("grants_issued", view.grants_issued) + "," +
                 field("grants_refused", view.grants_refused) + "," +
                 field("grants_fenced", view.grants_fenced));
        return 0;
    }
    if (command == "paths") {
        CTL_TRY(view, client.query_paths());
        std::string body = field("count", static_cast<std::uint64_t>(view.paths.size())) + "," +
                           boolean_field("truncated", view.truncated) + ",\"paths\":[";
        for (std::size_t i = 0; i < view.paths.size(); ++i) {
            const rnf::PathEntry& path = view.paths[i];
            if (i > 0) {
                body += ",";
            }
            body += "{" + field("id", path.id.to_hex()) + "," +
                    field("start_port", path.start_port.to_hex()) + "," +
                    field("end_port", path.end_port.to_hex()) + "," + field("hops", path.hops) +
                    "," + field("bottleneck", path.bottleneck.units) + "," +
                    boolean_field("bottleneck_known", path.bottleneck_known) + "," +
                    field("eligibility", std::string(rnf::to_string(path.eligibility))) + "}";
        }
        body += "]";
        emit(arguments, "paths", body);
        return 0;
    }
    if (command == "grants") {
        CTL_TRY(view, client.query_grants());
        std::string body = field("count", static_cast<std::uint64_t>(view.grants.size())) + ",\"grants\":[";
        for (std::size_t i = 0; i < view.grants.size(); ++i) {
            const rnf::GrantSummary& grant = view.grants[i];
            if (i > 0) {
                body += ",";
            }
            body += "{" + field("id", grant.id.to_hex()) + "," +
                    field("scope", quote(grant.scope.to_text())) + "," +
                    field("mode", std::string(rnf::to_string(grant.mode))) + "," +
                    field("capacity", grant.capacity.units) + "," +
                    field("generation", grant.generation.value) + "," +
                    field("epoch", grant.epoch.value) + "," +
                    field("incarnation", grant.incarnation.value) + "," +
                    field("fence", grant.fence) + "," +
                    field("state", std::string(rnf::to_string(grant.state))) + "," +
                    field("basis", quote(grant.basis.to_hex())) + "," +
                    field("expires_at_ms", grant.expires_at_ms) + "," +
                    field("reason", quote(grant.reason)) + "}";
        }
        body += "]";
        emit(arguments, "grants", body);
        return 0;
    }
    if (command == "resources") {
        rnf::ResourceQuery query;
        std::uint64_t number = 0;
        if (arguments.has("limit")) {
            if (!parse_u64(arguments.get("limit"), number) || number == 0) {
                emit_status(arguments, "resources",
                            rnf::Status(rnf::StatusCode::kInvalidArgument,
                                        "--limit needs a positive number"));
                return 2;
            }
            query.limit = static_cast<std::uint32_t>(number);
        }
        if (arguments.has("kind")) {
            rnf::ResourceKind kind = rnf::ResourceKind::kRack;
            if (!rnf::parse_resource_kind(arguments.get("kind"), kind)) {
                emit_status(arguments, "resources",
                            rnf::Status(rnf::StatusCode::kInvalidArgument, "unknown resource kind"));
                return 2;
            }
            query.kind_filter = static_cast<std::uint8_t>(kind);
        }
        CTL_TRY(view, client.query_resources(query));
        std::string body = field("count", static_cast<std::uint64_t>(view.resources.size())) + "," +
                           boolean_field("truncated", view.truncated) +
                           ",\"resources\":[";
        for (std::size_t i = 0; i < view.resources.size(); ++i) {
            const rnf::ResourceEntry& entry = view.resources[i];
            if (i > 0) {
                body += ",";
            }
            body += "{" + field("ref", quote(entry.ref.to_text())) + "," +
                    field("eligibility", std::string(rnf::to_string(entry.eligibility))) + "," +
                    field("availability", std::string(rnf::to_string(entry.availability))) + "," +
                    boolean_field("capacity_known", entry.capacity_known) + "," +
                    field("capacity", entry.capacity.units) + "," +
                    field("obligated", entry.obligated.units) + "," +
                    field("committed", entry.committed.units) + "," +
                    field("available", entry.available.units) + "," +
                    boolean_field("available_known", entry.available_known) + "," +
                    field("available_code", quote(rnf::to_string(entry.available_code))) + "," +
                    boolean_field("maintenance", entry.maintenance) + "}";
        }
        body += "]";
        emit(arguments, "resources", body);
        return 0;
    }
    if (command == "hello" || command == "state-peek") {
        emit(arguments, "hello",
             field("rack", state.rack.to_hex()) + "," +
                 field("generation", state.generation.value) + "," +
                 field("epoch", state.epoch.value) + "," +
                 field("incarnation", state.incarnation.value) + "," +
                 field("lifecycle", std::string(rnf::to_string(state.lifecycle))));
        return 0;
    }

    // -- control ------------------------------------------------------------
    if (command == "compose") {
        CTL_TRY(gen, required_number(arguments, "generation"));
        CTL_TRY(view, client.compose(rnf::TopologyGeneration{gen}));
        emit(arguments, "compose",
             field("code", quote(rnf::to_string(view.code))) + "," +
                 field("generation", view.generation.value) + "," +
                 field("epoch", view.epoch.value) + "," +
                 field("incarnation", view.incarnation.value) + "," +
                 field("snapshot_digest", quote(view.digest.to_hex())) + "," +
                 field("diagnostics", static_cast<std::uint64_t>(view.diagnostics)) + "," +
                 field("detail", quote(view.detail)));
        return view.code == rnf::StatusCode::kOk ? 0 : 1;
    }
    if (command == "lifecycle") {
        const std::string target_text = arguments.get("state", arguments.get("target"));
        rnf::LifecycleState target = rnf::LifecycleState::kActive;
        if (target_text.empty() || !rnf::parse_lifecycle_state(target_text, target)) {
            emit_status(arguments, "lifecycle",
                        rnf::Status(rnf::StatusCode::kInvalidArgument,
                                    "lifecycle needs --state <state>"));
            return 2;
        }
        CTL_TRY(view, client.set_lifecycle(target));
        emit(arguments, "lifecycle",
             field("code", quote(rnf::to_string(view.code))) + "," +
                 field("lifecycle", std::string(rnf::to_string(view.lifecycle))) + "," +
                 field("detail", quote(view.detail)));
        return view.code == rnf::StatusCode::kOk ? 0 : 1;
    }
    if (command == "checkpoint") {
        CTL_TRY(view, client.checkpoint());
        emit(arguments, "checkpoint", field("code", quote(rnf::to_string(view.code))) + "," +
                                          field("detail", quote(view.detail)));
        return view.code == rnf::StatusCode::kOk ? 0 : 1;
    }
    if (command == "shutdown") {
        CTL_TRY(view, client.shutdown());
        emit(arguments, "shutdown", field("code", quote(rnf::to_string(view.code))) + "," +
                                        field("detail", quote(view.detail)));
        return 0;
    }

    // -- evidence -----------------------------------------------------------
    auto submit_one = [&](rnf::EvidenceRecord& record, const char* label) -> int {
        const rnf::Result<rnf::SourceKind> kind = parse_source_kind(arguments);
        if (!kind.has_value()) {
            emit_status(arguments, label, kind.error());
            return 2;
        }
        record.provenance.kind = *kind;
        const rnf::Status valid = rnf::validate_evidence(record);
        if (!valid.ok()) {
            emit_status(arguments, label, valid);
            return 2;
        }
        const rnf::Result<rnf::EvidenceAckResponse> acknowledged =
            client.submit_evidence(std::vector<rnf::EvidenceRecord>{record});
        if (!acknowledged.has_value()) {
            emit_status(arguments, label, acknowledged.error());
            return 1;
        }
        if (acknowledged->entries.empty()) {
            emit_status(arguments, label,
                        rnf::Status(rnf::StatusCode::kInternal,
                                    "the daemon returned no acknowledgement"));
            return 1;
        }
        const rnf::EvidenceAckEntry& entry = acknowledged->entries.front();
        emit(arguments, label,
             field("outcome", std::string(rnf::to_string(entry.outcome))) + "," +
                 field("code", quote(rnf::to_string(entry.code))) + "," +
                 field("digest", quote(entry.digest.to_hex())) + "," +
                 field("detail", quote(entry.detail)));
        return entry.outcome == rnf::InsertOutcome::kRefused ? 1 : 0;
    };

    const EvidenceDefaults defaults = evidence_defaults(arguments, state);
    if (!defaults.ok) {
        emit_status(arguments, command,
                    rnf::Status(rnf::StatusCode::kInvalidArgument, defaults.error));
        return 2;
    }

    auto begin_record = [&](rnf::EvidenceKind kind, rnf::Durability durability) {
        rnf::EvidenceRecord record;
        record.kind = kind;
        record.durability = durability;
        record.generation = defaults.generation;
        record.provenance.source = defaults.source;
        record.provenance.sequence = defaults.sequence;
        record.provenance.observed_at_ms = default_sequence();
        return record;
    };

    if (command == "declare-member") {
        CTL_TRY(device, required_number(arguments, "device"));
        CTL_TRY(incarnation, required_number(arguments, "incarnation"));
        std::uint64_t rack_id = state.rack.value();
        if (arguments.has("member-rack")) {
            CTL_TRY(parsed, required_number(arguments, "member-rack"));
            rack_id = parsed;
        }
        rnf::MemberClaim claim;
        claim.rack = rnf::RackId(rack_id);
        claim.device = rnf::DeviceId(device);
        claim.incarnation = incarnation;
        rnf::EvidenceRecord record = begin_record(rnf::EvidenceKind::kMember,
                                                  rnf::Durability::kSticky);
        record.payload = claim;
        return submit_one(record, "declare-member");
    }
    if (command == "declare-device") {
        CTL_TRY(device, required_number(arguments, "device"));
        rnf::DeviceClaim claim;
        claim.device = rnf::DeviceId(device);
        claim.role = rnf::DeviceRole::kHost;
        if (arguments.has("role") && !rnf::parse_device_role(arguments.get("role"), claim.role)) {
            emit_status(arguments, command,
                        rnf::Status(rnf::StatusCode::kInvalidArgument, "unknown device role"));
            return 2;
        }
        if (arguments.has("capacity")) {
            CTL_TRY(capacity, required_number(arguments, "capacity"));
            claim.capacity = rnf::Capacity{capacity};
        } else {
            claim.capacity_known = false;
        }
        claim.availability = rnf::Availability::kUp;
        if (arguments.has("availability") &&
            !rnf::parse_availability(arguments.get("availability"), claim.availability)) {
            emit_status(arguments, command,
                        rnf::Status(rnf::StatusCode::kInvalidArgument, "unknown availability"));
            return 2;
        }
        rnf::EvidenceRecord record = begin_record(rnf::EvidenceKind::kDevice,
                                                  rnf::Durability::kSticky);
        record.payload = claim;
        return submit_one(record, "declare-device");
    }
    if (command == "declare-port") {
        CTL_TRY(port, required_number(arguments, "port"));
        CTL_TRY(device, required_number(arguments, "device"));
        CTL_TRY(index, required_number(arguments, "index"));
        if (index > 65535) {
            emit_status(arguments, command,
                        rnf::Status(rnf::StatusCode::kOutOfRange, "port index is out of range"));
            return 2;
        }
        rnf::PortClaim claim;
        claim.port = rnf::PortId(port);
        claim.device = rnf::DeviceId(device);
        claim.index = static_cast<std::uint16_t>(index);
        claim.role = rnf::PortRole::kAccess;
        if (arguments.has("port-role") &&
            !rnf::parse_port_role(arguments.get("port-role"), claim.role)) {
            emit_status(arguments, command,
                        rnf::Status(rnf::StatusCode::kInvalidArgument, "unknown port role"));
            return 2;
        }
        if (arguments.has("capacity")) {
            CTL_TRY(capacity, required_number(arguments, "capacity"));
            claim.capacity = rnf::Capacity{capacity};
        } else {
            claim.capacity_known = false;
        }
        claim.admin = rnf::AdminState::kEnabled;
        if (arguments.has("admin") &&
            !rnf::parse_admin_state(arguments.get("admin"), claim.admin)) {
            emit_status(arguments, command,
                        rnf::Status(rnf::StatusCode::kInvalidArgument, "unknown admin state"));
            return 2;
        }
        claim.availability = rnf::Availability::kUp;
        if (arguments.has("availability") &&
            !rnf::parse_availability(arguments.get("availability"), claim.availability)) {
            emit_status(arguments, command,
                        rnf::Status(rnf::StatusCode::kInvalidArgument, "unknown availability"));
            return 2;
        }
        rnf::EvidenceRecord record = begin_record(rnf::EvidenceKind::kPort,
                                                  rnf::Durability::kSticky);
        record.payload = claim;
        return submit_one(record, "declare-port");
    }
    if (command == "declare-link") {
        CTL_TRY(link, required_number(arguments, "link"));
        CTL_TRY(port_a, required_number(arguments, "port-a"));
        CTL_TRY(port_b, required_number(arguments, "port-b"));
        rnf::LinkClaim claim;
        claim.link = rnf::LinkId(link);
        claim.port_a = rnf::PortId(port_a);
        claim.port_b = rnf::PortId(port_b);
        claim.kind = rnf::LinkKind::kPhysical;
        if (arguments.has("kind") && !rnf::parse_link_kind(arguments.get("kind"), claim.kind)) {
            emit_status(arguments, command,
                        rnf::Status(rnf::StatusCode::kInvalidArgument, "unknown link kind"));
            return 2;
        }
        if (arguments.has("capacity")) {
            CTL_TRY(capacity, required_number(arguments, "capacity"));
            claim.capacity = rnf::Capacity{capacity};
        } else {
            claim.capacity_known = false;
        }
        claim.availability = rnf::Availability::kUp;
        if (arguments.has("availability") &&
            !rnf::parse_availability(arguments.get("availability"), claim.availability)) {
            emit_status(arguments, command,
                        rnf::Status(rnf::StatusCode::kInvalidArgument, "unknown availability"));
            return 2;
        }
        rnf::EvidenceRecord record = begin_record(rnf::EvidenceKind::kLink,
                                                  rnf::Durability::kSticky);
        record.payload = claim;
        return submit_one(record, "declare-link");
    }
    if (command == "attach") {
        CTL_TRY(attachment, required_number(arguments, "attachment"));
        CTL_TRY(device, required_number(arguments, "device"));
        CTL_TRY(port, required_number(arguments, "port"));
        rnf::AttachmentClaim claim;
        claim.attachment = rnf::AttachmentId(attachment);
        claim.device = rnf::DeviceId(device);
        claim.port = rnf::PortId(port);
        claim.host_key = arguments.get("host-key", "host");
        rnf::EvidenceRecord record = begin_record(rnf::EvidenceKind::kAttachment,
                                                  rnf::Durability::kSticky);
        record.payload = claim;
        return submit_one(record, "attach");
    }
    if (command == "maintenance") {
        rnf::MaintenanceClaim claim;
        if (!rnf::parse_maintenance_kind(arguments.get("action", "drain"), claim.action)) {
            emit_status(arguments, command,
                        rnf::Status(rnf::StatusCode::kInvalidArgument,
                                    "--action must be drain, maintenance or clear"));
            return 2;
        }
        CTL_TRY(target, rnf::parse_resource_ref(arguments.get("target")));
        claim.target_kind = target.kind;
        claim.target_id = target.id;
        claim.reason = arguments.get("reason", "operator directive");
        rnf::EvidenceRecord record = begin_record(rnf::EvidenceKind::kMaintenance,
                                                  rnf::Durability::kSticky);
        record.payload = claim;
        return submit_one(record, "maintenance");
    }
    if (command == "obligate") {
        CTL_TRY(obligation, required_number(arguments, "obligation"));
        CTL_TRY(capacity, required_number(arguments, "capacity"));
        CTL_TRY(holder, required_number(arguments, "holder"));
        CTL_TRY(target, rnf::parse_resource_ref(arguments.get("target")));
        rnf::ObligationClaim claim;
        claim.obligation = rnf::ObligationId(obligation);
        claim.target_kind = target.kind;
        claim.target_id = target.id;
        claim.capacity = rnf::Capacity{capacity};
        claim.holder = rnf::PrincipalId(holder);
        claim.authority = arguments.get("authority", "external-governance");
        rnf::EvidenceRecord record = begin_record(rnf::EvidenceKind::kObligation,
                                                  rnf::Durability::kSticky);
        record.payload = claim;
        return submit_one(record, "obligate");
    }

    // -- authority ----------------------------------------------------------
    if (command == "acquire") {
        const std::string request_text = arguments.get("request");
        if (request_text.size() != 32) {
            emit_status(arguments, command,
                        rnf::Status(rnf::StatusCode::kInvalidArgument,
                                    "--request needs 32 hex characters"));
            return 2;
        }
        const rnf::Result<rnf::Digest> bytes = rnf::Digest::from_hex(request_text + std::string(32, '0'));
        if (!bytes.has_value()) {
            emit_status(arguments, command, bytes.error());
            return 2;
        }
        rnf::AcquireRequest request;
        request.request.hi = 0;
        for (int i = 0; i < 16; ++i) {
            const char c = request_text[static_cast<std::size_t>(i)];
            const unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0')
                                    : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                                    : c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10)
                                                           : 0xFFU;
            if (digit == 0xFFU) {
                emit_status(arguments, command,
                            rnf::Status(rnf::StatusCode::kInvalidArgument,
                                        "--request needs hexadecimal characters"));
                return 2;
            }
            request.request.hi = (request.request.hi << 4U) | digit;
        }
        for (int i = 16; i < 32; ++i) {
            const char c = request_text[static_cast<std::size_t>(i)];
            const unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0')
                                    : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                                    : c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10)
                                                           : 0xFFU;
            if (digit == 0xFFU) {
                emit_status(arguments, command,
                            rnf::Status(rnf::StatusCode::kInvalidArgument,
                                        "--request needs hexadecimal characters"));
                return 2;
            }
            request.request.lo = (request.request.lo << 4U) | digit;
        }
        CTL_TRY(principal, required_number(arguments, "principal"));
        request.principal = rnf::PrincipalId(principal);
        CTL_TRY(scope, rnf::parse_resource_ref(arguments.get("scope")));
        request.scope = scope;
        if (arguments.has("mode") && !rnf::parse_grant_mode(arguments.get("mode"), request.mode)) {
            emit_status(arguments, command,
                        rnf::Status(rnf::StatusCode::kInvalidArgument,
                                    "--mode must be shared or exclusive"));
            return 2;
        }
        if (arguments.has("capacity")) {
            CTL_TRY(capacity, required_number(arguments, "capacity"));
            request.capacity = rnf::Capacity{capacity};
        }
        if (arguments.has("ttl-ms")) {
            CTL_TRY(ttl, required_number(arguments, "ttl-ms"));
            request.ttl_ms = ttl;
        }
        const rnf::Result<rnf::GrantResponse> response = client.acquire(request);
        if (!response.has_value()) {
            emit_status(arguments, command, response.error());
            return 1;
        }
        emit(arguments, "acquire",
             field("code", quote(rnf::to_string(response->code))) + "," +
                 field("grant", response->grant.id.to_hex()) + "," +
                 field("fence", response->grant.fence) + "," +
                 field("incarnation", response->grant.incarnation.value) + "," +
                 field("basis", quote(response->grant.basis.to_hex())) + "," +
                 field("state", std::string(rnf::to_string(response->grant.state))) + "," +
                 field("expires_at_ms", response->grant.expires_at_ms) + "," +
                 field("detail", quote(response->detail)));
        return response->code == rnf::StatusCode::kOk ? 0 : 1;
    }
    if (command == "lookup") {
        const std::string request_text = arguments.get("request");
        if (request_text.size() != 32) {
            emit_status(arguments, command,
                        rnf::Status(rnf::StatusCode::kInvalidArgument,
                                    "--request needs 32 hex characters"));
            return 2;
        }
        rnf::RequestId id;
        for (int i = 0; i < 32; ++i) {
            const char c = request_text[static_cast<std::size_t>(i)];
            const unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0')
                                    : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
                                    : c >= 'A' && c <= 'F' ? static_cast<unsigned>(c - 'A' + 10)
                                                           : 0xFFU;
            if (digit == 0xFFU) {
                emit_status(arguments, command,
                            rnf::Status(rnf::StatusCode::kInvalidArgument,
                                        "--request needs hexadecimal characters"));
                return 2;
            }
            if (i < 16) {
                id.hi = (id.hi << 4U) | digit;
            } else {
                id.lo = (id.lo << 4U) | digit;
            }
        }
        const rnf::Result<rnf::LookupResponse> response = client.lookup(id);
        if (!response.has_value()) {
            emit_status(arguments, command, response.error());
            return 1;
        }
        emit(arguments, "lookup",
             field("code", quote(rnf::to_string(response->code))) + "," +
                 boolean_field("remembered", response->remembered) + "," +
                 field("outcome", quote(rnf::to_string(response->outcome_code))) + "," +
                 field("grant", response->grant.to_hex()) + "," +
                 field("fence", response->fence) + "," +
                 field("incarnation", response->incarnation.value) + "," +
                 field("detail", quote(response->detail)));
        return 0;
    }
    if (command == "renew" || command == "release" || command == "validate") {
        rnf::LeaseToken token;
        CTL_TRY(grant, required_number(arguments, "grant"));
        CTL_TRY(fence, required_number(arguments, "fence"));
        CTL_TRY(incarnation, required_number(arguments, "incarnation"));
        token.grant = rnf::GrantId(grant);
        token.fence = fence;
        token.incarnation = rnf::ControllerIncarnation{incarnation};
        CTL_TRY(basis, rnf::Digest::from_hex(arguments.get("basis")));
        token.scope_basis = basis;
        if (command == "renew") {
            std::uint64_t ttl = 60000;
            if (arguments.has("ttl-ms")) {
                CTL_TRY(parsed, required_number(arguments, "ttl-ms"));
                ttl = parsed;
            }
            const rnf::Result<rnf::GrantResponse> response = client.renew(token, ttl);
            if (!response.has_value()) {
                emit_status(arguments, command, response.error());
                return 1;
            }
            emit(arguments, "renew", field("code", quote(rnf::to_string(response->code))) + "," +
                                         field("expires_at_ms", response->grant.expires_at_ms) +
                                         "," + field("detail", quote(response->detail)));
            return response->code == rnf::StatusCode::kOk ? 0 : 1;
        }
        if (command == "release") {
            const rnf::Result<rnf::AckResponse> response = client.release(token);
            if (!response.has_value()) {
                emit_status(arguments, command, response.error());
                return 1;
            }
            emit(arguments, "release", field("code", quote(rnf::to_string(response->code))) + "," +
                                           field("detail", quote(response->detail)));
            return response->code == rnf::StatusCode::kOk ? 0 : 1;
        }
        const rnf::Result<rnf::GrantResponse> response = client.validate(token);
        if (!response.has_value()) {
            emit_status(arguments, command, response.error());
            return 1;
        }
        emit(arguments, "validate",
             field("code", quote(rnf::to_string(response->code))) + "," +
                 field("state", std::string(rnf::to_string(response->grant.state))) + "," +
                 field("detail", quote(response->detail)));
        return response->code == rnf::StatusCode::kOk ? 0 : 1;
    }

    emit_status(arguments, "unknown",
                rnf::Status(rnf::StatusCode::kInvalidArgument,
                            "unknown command " + command));
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    rnf::ensure_socket_runtime_noexcept();
    Arguments arguments = parse_arguments(argc, argv);
    if (!arguments.valid) {
        std::cerr << "rnfctl: " << arguments.error << "\n";
        print_usage();
        return 2;
    }
    const rnf::Result<rnf::Endpoint> endpoint = parse_endpoint(arguments.endpoint);
    if (!endpoint.has_value()) {
        std::cerr << "rnfctl: " << endpoint.error().describe() << "\n";
        return 2;
    }

    rnf::ClientOptions options;
    options.io_timeout_ms = arguments.timeout_ms;
    rnf::Result<rnf::Client> connected = rnf::Client::connect(*endpoint, options);
    if (!connected.has_value()) {
        std::cerr << "rnfctl: " << connected.error().describe() << "\n";
        return 1;
    }
    rnf::Client client = std::move(*connected);
    const rnf::Result<rnf::HelloResponse> hello = client.hello(1);
    if (!hello.has_value()) {
        std::cerr << "rnfctl: " << hello.error().describe() << "\n";
        return 1;
    }
    if (!hello->accepted) {
        std::cerr << "rnfctl: the daemon refused the session: " << hello->detail << "\n";
        return 1;
    }
    const rnf::Result<rnf::StateResponse> state = client.query_state();
    if (!state.has_value()) {
        std::cerr << "rnfctl: " << state.error().describe() << "\n";
        return 1;
    }

    const int code = run_command(arguments, client, *state);
    client.close();
    return code;
}
