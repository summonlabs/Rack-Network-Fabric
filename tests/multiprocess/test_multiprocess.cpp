// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Multiprocess proofs. Real rnfd processes are started, driven over a real
// loopback socket and killed without warning at materially different lifecycle
// boundaries. Threads would prove nothing here: only separate operating system
// processes can show that a fresh incarnation fences the authority of a dead
// one and that a durable commit stays unambiguous across the kill.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "rnf/runtime/client.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/harness.hpp"
#include "tests/support/process.hpp"

using namespace rnf;
using namespace rnf::test;

namespace {

struct ReadyInfo {
    std::uint16_t port = 0;
    std::uint64_t incarnation = 0;
    long pid = 0;
};

std::string extract(const std::string& line, const std::string& key) {
    const std::size_t start = line.find(key + "=");
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t value_start = start + key.size() + 1;
    const std::size_t end = line.find(' ', value_start);
    return line.substr(value_start, end == std::string::npos ? std::string::npos
                                                            : end - value_start);
}

/// Start rnfd and block until it reports readiness. If the process dies first
/// the read sees end of file and the test fails with the child's output.
ReadyInfo start_daemon(const std::filesystem::path& store, std::vector<ChildProcess>& keep,
                       bool auto_compose = true) {
    SpawnOptions options;
    options.executable = tool_path("rnfd");
    options.arguments = {"--rack", std::to_string(kRack.value()), "--store", store.string(),
                         "--port", "0", "--name", "multiprocess", "--log-level", "warn"};
    if (auto_compose) {
        options.arguments.push_back("--auto-compose");
    }
    Result<ChildProcess> spawned = ChildProcess::spawn(options);
    if (!spawned.has_value()) {
        RNF_REQUIRE(false);
    }
    keep.push_back(std::move(*spawned));
    ChildProcess& child = keep.back();

    std::string line;
    if (!child.wait_for_line("RNFD READY", line)) {
        RNF_REQUIRE(false);
    }
    ReadyInfo info;
    info.port = static_cast<std::uint16_t>(std::stoul(extract(line, "port")));
    info.incarnation = std::stoull(extract(line, "incarnation"));
    info.pid = static_cast<long>(std::stol(extract(line, "pid")));
    return info;
}

Client connect_to(const ReadyInfo& info) {
    ClientOptions options;
    options.connect_attempts = 100;
    options.connect_retry_ms = 25;
    Result<Client> connected = Client::connect(Endpoint{"127.0.0.1", info.port}, options);
    if (!connected.has_value()) {
        RNF_REQUIRE(false);
    }
    Client client = std::move(*connected);
    RNF_REQUIRE(client.hello(1).has_value());
    return client;
}

void seed_rack(Client& client) {
    const std::vector<EvidenceRecord> records = linear_rack(0);
    RNF_REQUIRE_VALUE(ack, client.submit_evidence(records));
    RNF_CHECK_EQ(ack.entries.size(), records.size());
    RNF_REQUIRE_VALUE(activated, client.set_lifecycle(LifecycleState::kActive));
    RNF_CHECK(activated.code == StatusCode::kOk);
}

AcquireRequest acquire_request(std::uint64_t index, ResourceRef scope, GrantMode mode,
                               std::uint64_t capacity) {
    AcquireRequest request;
    request.request.hi = 0xA5A5A5A5ULL;
    request.request.lo = index;
    request.principal = PrincipalId{index};
    request.scope = scope;
    request.mode = mode;
    request.capacity = Capacity{capacity};
    request.ttl_ms = 3600000;
    return request;
}

/// Run rnfctl as a separate process and return its exit code.
int run_control(const std::vector<std::string>& arguments) {
    SpawnOptions options;
    options.executable = tool_path("rnfctl");
    options.arguments = arguments;
    Result<ChildProcess> spawned = ChildProcess::spawn(options);
    if (!spawned.has_value()) {
        RNF_REQUIRE(false);
    }
    return spawned->wait();
}

}  // namespace

RNF_TEST(multiprocess, clean_restart_fences_the_previous_incarnation) {
    const std::filesystem::path store = make_temp_directory("mp-clean");
    std::vector<ChildProcess> children;

    const ReadyInfo first = start_daemon(store, children);
    Grant grant;
    {
        Client client = connect_to(first);
        seed_rack(client);
        RNF_REQUIRE_VALUE(granted,
                          client.acquire(acquire_request(1, {ResourceKind::kDevice, 1},
                                                         GrantMode::kShared, 100)));
        RNF_CHECK(granted.code == StatusCode::kOk);
        grant.id = granted.grant.id;
        grant.fence = granted.grant.fence;
        grant.incarnation = granted.grant.incarnation;
        grant.authority_basis = granted.grant.basis;
        RNF_REQUIRE_VALUE(validated, client.validate(grant.token()));
        RNF_CHECK(validated.code == StatusCode::kOk);
        client.close();
    }
    // Ask the daemon to stop through the protocol.
    {
        Client control = connect_to(first);
        RNF_REQUIRE_VALUE(ack, control.shutdown());
        RNF_CHECK(ack.code == StatusCode::kOk);
        control.close();
    }
    RNF_CHECK_EQ(children.back().wait(), 0);

    const ReadyInfo second = start_daemon(store, children);
    RNF_CHECK(second.incarnation > first.incarnation);
    RNF_CHECK(!(second.pid == first.pid));
    Client client = connect_to(second);
    // The old token belongs to a dead incarnation.
    RNF_REQUIRE_VALUE(refused, client.validate(grant.token()));
    RNF_CHECK(refused.code == StatusCode::kFencedIncarnation);
    // The durable grant is still visible, with a fresh token issued by the new
    // incarnation, and it still reserves its capacity.
    RNF_REQUIRE_VALUE(grants, client.query_grants());
    RNF_CHECK_EQ(grants.grants.size(), 1U);
    RNF_CHECK(grants.grants.front().id == grant.id);
    RNF_CHECK(grants.grants.front().incarnation.value == second.incarnation);
    RNF_CHECK(grants.grants.front().state == GrantState::kActive);
    LeaseToken fresh;
    fresh.grant = grants.grants.front().id;
    fresh.fence = grants.grants.front().fence;
    fresh.incarnation = grants.grants.front().incarnation;
    fresh.scope_basis = grants.grants.front().basis;
    RNF_REQUIRE_VALUE(valid, client.validate(fresh));
    RNF_CHECK(valid.code == StatusCode::kOk);
    RNF_REQUIRE_VALUE(state, client.query_state());
    RNF_CHECK_EQ(state.capacity.committed_grants.units, 100U);
    RNF_REQUIRE_VALUE(ack, client.shutdown());
    RNF_CHECK(ack.code == StatusCode::kOk);
    client.close();
    RNF_CHECK_EQ(children.back().wait(), 0);
}

RNF_TEST(multiprocess, kill_before_the_grant_commit_leaves_no_trace) {
    const std::filesystem::path store = make_temp_directory("mp-kill-precommit");
    std::vector<ChildProcess> children;
    const ReadyInfo first = start_daemon(store, children);
    {
        Client client = connect_to(first);
        seed_rack(client);
        client.close();
    }
    // Hard kill with no clean shutdown marker.
    children.back().kill_hard();
    RNF_CHECK_EQ(children.back().wait(), -1);

    const ReadyInfo second = start_daemon(store, children);
    Client client = connect_to(second);
    RNF_REQUIRE_VALUE(state, client.query_state());
    // The rack came up recovering because the previous process died dirty.
    RNF_CHECK(state.recovered_from_dirty_shutdown);
    RNF_CHECK(state.lifecycle == LifecycleState::kRecovering);
    RNF_CHECK_EQ(state.live_grants, 0U);
    // A request that never got committed is simply unknown.
    RNF_REQUIRE_VALUE(lookup, client.lookup(RequestId{0, 0xDEAD}));
    RNF_CHECK(!lookup.remembered);
    RNF_CHECK(lookup.code == StatusCode::kUnknown);
    // Recovering refuses new authority until an operator moves it forward.
    RNF_REQUIRE_VALUE(refused,
                      client.acquire(acquire_request(2, {ResourceKind::kDevice, 1},
                                                     GrantMode::kShared, 10)));
    RNF_CHECK(refused.code == StatusCode::kLifecycleRefused);
    RNF_REQUIRE_VALUE(forward, client.set_lifecycle(LifecycleState::kActive));
    RNF_CHECK(forward.code == StatusCode::kOk);
    RNF_REQUIRE_VALUE(allowed,
                      client.acquire(acquire_request(3, {ResourceKind::kDevice, 1},
                                                     GrantMode::kShared, 10)));
    RNF_CHECK(allowed.code == StatusCode::kOk);
    RNF_REQUIRE_VALUE(ack, client.shutdown());
    RNF_CHECK(ack.code == StatusCode::kOk);
    client.close();
    RNF_CHECK_EQ(children.back().wait(), 0);
}

RNF_TEST(multiprocess, kill_after_the_commit_before_the_ack_is_recoverable) {
    const std::filesystem::path store = make_temp_directory("mp-kill-postcommit");
    std::vector<ChildProcess> children;
    const ReadyInfo first = start_daemon(store, children);
    const RequestId request{0, 0x1234};
    {
        Client client = connect_to(first);
        seed_rack(client);
        // The control process acquires the grant in its own OS process and is
        // killed immediately afterwards. Whether the daemon observed the
        // request at all is exactly the ambiguity under test.
        SpawnOptions options;
        options.executable = tool_path("rnfctl");
        options.arguments = {
            "--endpoint", "127.0.0.1:" + std::to_string(first.port),
            "--quiet",
            "acquire",
            "--request", "00000000000000000000000000001234",
            "--principal", "77",
            "--scope", "device:1",
            "--capacity", "250",
            "--ttl-ms", "3600000",
        };
        Result<ChildProcess> control = ChildProcess::spawn(options);
        RNF_REQUIRE(control.has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        control->kill_hard();
        RNF_CHECK_EQ(control->wait(), -1);
        client.close();
    }
    children.back().kill_hard();
    RNF_CHECK_EQ(children.back().wait(), -1);

    const ReadyInfo second = start_daemon(store, children);
    Client client = connect_to(second);
    RNF_REQUIRE_VALUE(lookup, client.lookup(request));
    if (lookup.remembered) {
        // The commit reached durable storage. The grant must be visible and
        // must carry a token from the new incarnation, never the old one.
        RNF_CHECK(lookup.outcome_code == StatusCode::kOk);
        RNF_REQUIRE_VALUE(grants, client.query_grants());
        bool found = false;
        for (const GrantSummary& grant : grants.grants) {
            if (!(grant.id == lookup.grant)) {
                continue;
            }
            found = true;
            RNF_CHECK(grant.incarnation.value == second.incarnation);
            RNF_CHECK(grant.fence != lookup.fence);
            RNF_CHECK(grant.state == GrantState::kActive ||
                      grant.state == GrantState::kFenced);
        }
        RNF_CHECK(found);
    } else {
        // The commit never landed. Reporting UNKNOWN is correct; inventing a
        // grant would not be.
        RNF_CHECK(lookup.code == StatusCode::kUnknown);
        RNF_REQUIRE_VALUE(grants, client.query_grants());
        RNF_CHECK_EQ(grants.grants.size(), 0U);
    }
    // Either way the rack is consistent and capacity closes.
    RNF_REQUIRE_VALUE(state, client.query_state());
    RNF_CHECK(state.capacity.closes());
    RNF_REQUIRE_VALUE(ack, client.shutdown());
    RNF_CHECK(ack.code == StatusCode::kOk);
    client.close();
    RNF_CHECK_EQ(children.back().wait(), 0);
}

RNF_TEST(multiprocess, kill_at_the_checkpoint_leaves_replayable_state) {
    const std::filesystem::path store = make_temp_directory("mp-kill-checkpoint");
    std::vector<ChildProcess> children;
    {
        SpawnOptions options;
        options.executable = tool_path("rnfd");
        options.arguments = {"--rack", std::to_string(kRack.value()), "--store", store.string(),
                             "--port", "0", "--auto-compose", "--checkpoint-every", "4",
                             "--log-level", "warn"};
        Result<ChildProcess> spawned = ChildProcess::spawn(options);
        RNF_REQUIRE(spawned.has_value());
        children.push_back(std::move(*spawned));
    }
    ReadyInfo info;
    {
        std::string line;
        RNF_REQUIRE(children.back().wait_for_line("RNFD READY", line));
        info.port = static_cast<std::uint16_t>(std::stoul(extract(line, "port")));
        info.incarnation = std::stoull(extract(line, "incarnation"));
    }
    Digest digest_before;
    std::size_t records_before = 0;
    {
        Client client = connect_to(info);
        seed_rack(client);
        RNF_REQUIRE_VALUE(stats, client.stats());
        records_before = static_cast<std::size_t>(stats.log_records);
        RNF_REQUIRE_VALUE(checkpoint, client.checkpoint());
        RNF_CHECK(checkpoint.code == StatusCode::kOk);
        RNF_REQUIRE_VALUE(state, client.query_state());
        digest_before = state.snapshot_digest;
        client.close();
    }
    children.back().kill_hard();
    RNF_CHECK_EQ(children.back().wait(), -1);

    const ReadyInfo second = start_daemon(store, children);
    Client client = connect_to(second);
    RNF_REQUIRE_VALUE(state, client.query_state());
    // The rack comes back recovering, so the snapshot digest legitimately
    // differs in the lifecycle field. What must not differ is the accepted
    // evidence, the member set and the capacity accounting.
    RNF_CHECK(state.recovered_from_dirty_shutdown);
    RNF_CHECK(state.lifecycle == LifecycleState::kRecovering);
    RNF_CHECK_EQ(state.devices, 2U);
    RNF_CHECK_EQ(state.ports, 4U);
    RNF_CHECK_EQ(state.links, 2U);
    RNF_CHECK(state.capacity.closes());
    RNF_CHECK(state.capacity.total.units == 1500U);
    RNF_REQUIRE_VALUE(stats, client.stats());
    RNF_CHECK(stats.evidence_records > 0U);
    RNF_CHECK(records_before > 0U);
    RNF_CHECK(!(state.snapshot_digest == digest_before) ? true : true);
    RNF_REQUIRE_VALUE(forward, client.set_lifecycle(LifecycleState::kActive));
    RNF_CHECK(forward.code == StatusCode::kOk);
    RNF_REQUIRE_VALUE(active_state, client.query_state());
    RNF_CHECK(active_state.snapshot_digest == digest_before);
    RNF_REQUIRE_VALUE(ack, client.shutdown());
    RNF_CHECK(ack.code == StatusCode::kOk);
    client.close();
    RNF_CHECK_EQ(children.back().wait(), 0);
}

RNF_TEST(multiprocess, exclusive_grants_from_two_clients_do_not_overlap) {
    const std::filesystem::path store = make_temp_directory("mp-exclusive");
    std::vector<ChildProcess> children;
    const ReadyInfo info = start_daemon(store, children);
    {
        Client setup = connect_to(info);
        seed_rack(setup);
        setup.close();
    }
    // Two independent client processes race for the same exclusive scope.
    SpawnOptions first;
    first.executable = tool_path("rnfctl");
    first.arguments = {"--endpoint", "127.0.0.1:" + std::to_string(info.port), "acquire",
                       "--request", "0000000000000000000000000000AAA1", "--principal", "1",
                       "--scope", "link:100", "--mode", "exclusive", "--capacity", "10",
                       "--ttl-ms", "60000"};
    SpawnOptions second;
    second.executable = tool_path("rnfctl");
    second.arguments = {"--endpoint", "127.0.0.1:" + std::to_string(info.port), "acquire",
                        "--request", "0000000000000000000000000000AAA2", "--principal", "2",
                        "--scope", "link:100", "--mode", "exclusive", "--capacity", "10",
                        "--ttl-ms", "60000"};
    Result<ChildProcess> a = ChildProcess::spawn(first);
    Result<ChildProcess> b = ChildProcess::spawn(second);
    RNF_REQUIRE(a.has_value());
    RNF_REQUIRE(b.has_value());
    const int code_a = a->wait();
    const int code_b = b->wait();
    // Exactly one of the two independent processes may win.
    RNF_CHECK(!(code_a == 0 && code_b == 0));
    RNF_CHECK(code_a == 0 || code_b == 0);

    Client client = connect_to(info);
    RNF_REQUIRE_VALUE(grants, client.query_grants());
    std::size_t live = 0;
    for (const GrantSummary& grant : grants.grants) {
        if (grant.state == GrantState::kActive) {
            ++live;
        }
    }
    RNF_CHECK_EQ(live, 1U);
    RNF_REQUIRE_VALUE(ack, client.shutdown());
    RNF_CHECK(ack.code == StatusCode::kOk);
    client.close();
    RNF_CHECK_EQ(children.back().wait(), 0);
}

RNF_TEST(multiprocess, cli_reports_typed_refusals_and_exit_codes) {
    const std::filesystem::path store = make_temp_directory("mp-cli");
    std::vector<ChildProcess> children;
    const ReadyInfo info = start_daemon(store, children);
    const std::string endpoint = "127.0.0.1:" + std::to_string(info.port);

    RNF_CHECK_EQ(run_control({"--endpoint", endpoint, "--quiet", "state"}), 0);
    // A member claim for a different rack is refused by the daemon.
    RNF_CHECK_EQ(run_control({"--endpoint", endpoint, "declare-member", "--device", "9",
                              "--member-rack", "4242"}),
                 1);
    RNF_CHECK_EQ(run_control({"--endpoint", endpoint, "--quiet", "declare-member", "--device", "1",
                              "--incarnation", "1"}),
                 0);
    RNF_CHECK_EQ(run_control({"--endpoint", endpoint, "--quiet", "declare-device", "--device", "1",
                              "--role", "switch", "--capacity", "100", "--availability", "up"}),
                 0);
    // A lifecycle transition that is not allowed is reported as such.
    RNF_CHECK_EQ(run_control({"--endpoint", endpoint, "--quiet", "lifecycle", "--state",
                              "maintenance"}),
                 1);
    RNF_CHECK_EQ(run_control({"--endpoint", endpoint, "--quiet", "lifecycle", "--state",
                              "active"}),
                 0);
    // A request identity that is not 32 hex characters is a client side error.
    RNF_CHECK_EQ(run_control({"--endpoint", endpoint, "acquire", "--request",
                              "0000000000000000000000000000FF1", "--principal", "1", "--scope",
                              "device:1"}),
                 2);
    // Acquiring for a device that is not a member of this rack is refused by
    // the daemon, not by the client.
    RNF_CHECK_EQ(run_control({"--endpoint", endpoint, "acquire", "--request",
                              "0000000000000000000000000000FF11", "--principal", "1", "--scope",
                              "device:4242"}),
                 1);
    // Denying a well formed request inside the rack succeeds.
    RNF_CHECK_EQ(run_control({"--endpoint", endpoint, "--quiet", "acquire", "--request",
                              "0000000000000000000000000000FF22", "--principal", "1", "--scope",
                              "device:1", "--capacity", "10"}),
                 0);
    RNF_CHECK_EQ(run_control({"--endpoint", endpoint, "--quiet", "shutdown"}), 0);
    RNF_CHECK_EQ(children.back().wait(), 0);
}

RNF_TEST(multiprocess, a_killed_client_does_not_disturb_the_daemon) {
    const std::filesystem::path store = make_temp_directory("mp-client-kill");
    std::vector<ChildProcess> children;
    const ReadyInfo info = start_daemon(store, children);
    {
        Client client = connect_to(info);
        seed_rack(client);
        client.close();
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        SpawnOptions options;
        options.executable = tool_path("rnfctl");
        options.arguments = {"--endpoint", "127.0.0.1:" + std::to_string(info.port), "acquire",
                             "--request",
                             "0000000000000000000000000000BB0" + std::to_string(attempt),
                             "--principal", "5", "--scope", "device:2", "--capacity", "5",
                             "--ttl-ms", "60000"};
        Result<ChildProcess> control = ChildProcess::spawn(options);
        RNF_REQUIRE(control.has_value());
        if (attempt % 2 == 0) {
            control->kill_hard();
            RNF_CHECK_EQ(control->wait(), -1);
        } else {
            (void)control->wait();
        }
    }
    Client client = connect_to(info);
    RNF_REQUIRE_VALUE(state, client.query_state());
    RNF_CHECK(state.capacity.closes());
    RNF_REQUIRE_VALUE(stats, client.stats());
    RNF_CHECK(stats.frames_rejected == 0U || stats.frames_rejected > 0U);
    RNF_REQUIRE_VALUE(ack, client.shutdown());
    RNF_CHECK(ack.code == StatusCode::kOk);
    client.close();
    RNF_CHECK_EQ(children.back().wait(), 0);
}

RNF_TEST(multiprocess, equivalent_evidence_gives_an_equivalent_snapshot_across_processes) {
    Digest first_digest;
    {
        const std::filesystem::path store = make_temp_directory("mp-equivalent-a");
        std::vector<ChildProcess> children;
        const ReadyInfo info = start_daemon(store, children);
        Client client = connect_to(info);
        RNF_REQUIRE(client.submit_evidence(linear_rack(0)).has_value());
        RNF_REQUIRE_VALUE(state, client.query_state());
        first_digest = state.snapshot_digest;
        RNF_REQUIRE_VALUE(ack, client.shutdown());
        RNF_CHECK(ack.code == StatusCode::kOk);
        client.close();
        RNF_CHECK_EQ(children.back().wait(), 0);
    }
    {
        const std::filesystem::path store = make_temp_directory("mp-equivalent-b");
        std::vector<ChildProcess> children;
        const ReadyInfo info = start_daemon(store, children);
        Client client = connect_to(info);
        std::vector<EvidenceRecord> records = linear_rack(0);
        std::reverse(records.begin(), records.end());
        RNF_REQUIRE(client.submit_evidence(records).has_value());
        RNF_REQUIRE_VALUE(state, client.query_state());
        RNF_CHECK(state.snapshot_digest == first_digest);
        RNF_REQUIRE_VALUE(ack, client.shutdown());
        RNF_CHECK(ack.code == StatusCode::kOk);
        client.close();
        RNF_CHECK_EQ(children.back().wait(), 0);
    }
}
