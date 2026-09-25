// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Integration tests: the daemon core, the loopback server and the client
// library exercised together over a real TCP connection in one process.

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "rnf/runtime/client.hpp"
#include "rnf/runtime/daemon.hpp"
#include "rnf/runtime/server.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/harness.hpp"

using namespace rnf;
using namespace rnf::test;

namespace {

/// A running daemon plus server plus a connected client, torn down in the
/// right order regardless of how the test exits.
class Rig {
public:
    explicit Rig(const DaemonConfig& config) : core_(config), server_(core_, ServerConfig{}) {}

    bool start() {
        if (!core_.start().ok()) {
            return false;
        }
        if (!server_.bind("127.0.0.1", 0).ok()) {
            return false;
        }
        runner_ = std::thread([this]() { (void)server_.run(&stop_); });
        ClientOptions options;
        options.connect_attempts = 60;
        options.connect_retry_ms = 25;
        Result<Client> connected =
            Client::connect(Endpoint{"127.0.0.1", server_.port()}, options);
        if (!connected.has_value()) {
            return false;
        }
        client_.emplace(std::move(*connected));
        return client_->hello(1).has_value();
    }

    ~Rig() {
        if (client_.has_value()) {
            client_->close();
        }
        stop_.store(true, std::memory_order_release);
        server_.request_stop();
        if (runner_.joinable()) {
            runner_.join();
        }
        (void)core_.stop(1);
    }

    [[nodiscard]] Client& client() { return *client_; }
    [[nodiscard]] DaemonCore& core() { return core_; }

private:
    DaemonCore core_;
    DaemonServer server_;
    std::atomic<bool> stop_{false};
    std::thread runner_;
    std::optional<Client> client_;
};

DaemonConfig rig_config() {
    DaemonConfig config;
    config.rack = linear_rack_config();
    config.rack.name = "integration";
    config.auto_compose = true;
    config.default_ttl_ms = 60000;
    return config;
}

}  // namespace

RNF_TEST(integration, full_workflow_over_tcp) {
    Rig rig(rig_config());
    RNF_REQUIRE(rig.start());
    Client& client = rig.client();

    const std::vector<EvidenceRecord> records = linear_rack(0);
    RNF_REQUIRE_VALUE(ack, client.submit_evidence(records));
    RNF_CHECK_EQ(ack.entries.size(), records.size());
    for (const EvidenceAckEntry& entry : ack.entries) {
        RNF_CHECK(!(entry.outcome == InsertOutcome::kRefused));
    }

    RNF_REQUIRE_VALUE(activated, client.set_lifecycle(LifecycleState::kActive));
    RNF_CHECK(activated.code == StatusCode::kOk);

    RNF_REQUIRE_VALUE(state, client.query_state());
    RNF_CHECK(state.rack == kRack);
    RNF_CHECK(state.lifecycle == LifecycleState::kActive);
    RNF_CHECK_EQ(state.devices, 2U);
    RNF_CHECK_EQ(state.ports, 4U);
    RNF_CHECK_EQ(state.links, 2U);
    RNF_CHECK_EQ(state.capacity.total.units, 1500U);
    RNF_CHECK(state.capacity.closes());

    RNF_REQUIRE_VALUE(resources, client.query_resources(ResourceQuery{}));
    RNF_CHECK(resources.truncated == false);
    RNF_CHECK_EQ(resources.resources.size(), state.devices + state.ports + state.links +
                                                state.attachments + state.paths + 1);
    for (const ResourceEntry& entry : resources.resources) {
        if (entry.ref.kind == ResourceKind::kDevice) {
            RNF_CHECK(entry.available_known);
        }
    }

    RNF_REQUIRE_VALUE(paths, client.query_paths());
    RNF_CHECK(!paths.paths.empty());
    for (const PathEntry& path : paths.paths) {
        RNF_CHECK(path.eligibility == Eligibility::kEligible);
        RNF_CHECK(path.hops >= 1U);
    }

    AcquireRequest acquire;
    acquire.request.hi = 1;
    acquire.request.lo = 1;
    acquire.principal = PrincipalId{42};
    acquire.scope = ResourceRef{ResourceKind::kDevice, 2};
    acquire.mode = GrantMode::kShared;
    acquire.capacity = Capacity{100};
    acquire.ttl_ms = 30000;
    RNF_REQUIRE_VALUE(granted, client.acquire(acquire));
    RNF_CHECK(granted.code == StatusCode::kOk);
    RNF_CHECK(granted.grant.id.is_set());
    RNF_CHECK_EQ(granted.grant.incarnation.value, state.incarnation.value);

    LeaseToken token;
    token.grant = granted.grant.id;
    token.fence = granted.grant.fence;
    token.incarnation = granted.grant.incarnation;
    token.scope_basis = granted.grant.basis;

    RNF_REQUIRE_VALUE(validated, client.validate(token));
    RNF_CHECK(validated.code == StatusCode::kOk);
    RNF_REQUIRE_VALUE(renewed, client.renew(token, 45000));
    RNF_CHECK(renewed.code == StatusCode::kOk);
    RNF_CHECK(renewed.grant.expires_at_ms >= granted.grant.expires_at_ms);

    RNF_REQUIRE_VALUE(grants, client.query_grants());
    RNF_CHECK_EQ(grants.grants.size(), 1U);
    RNF_CHECK(grants.grants.front().id == granted.grant.id);

    RNF_REQUIRE_VALUE(committed, client.query_state());
    RNF_CHECK_EQ(committed.capacity.committed_grants.units, 100U);

    RNF_REQUIRE_VALUE(released, client.release(token));
    RNF_CHECK(released.code == StatusCode::kOk);
    RNF_REQUIRE_VALUE(freed, client.query_state());
    RNF_CHECK_EQ(freed.capacity.committed_grants.units, 0U);

    RNF_REQUIRE_VALUE(stats, client.stats());
    RNF_CHECK(stats.requests_served >= 10U);
    RNF_CHECK(stats.bytes_sent > 0U);
    RNF_CHECK(stats.bytes_received > 0U);

    RNF_REQUIRE_VALUE(checkpoint, client.checkpoint());
    RNF_CHECK(checkpoint.code == StatusCode::kUnsupported);  // no store configured
}

RNF_TEST(integration, idempotent_acquire_over_tcp) {
    Rig rig(rig_config());
    RNF_REQUIRE(rig.start());
    Client& client = rig.client();
    RNF_REQUIRE(client.submit_evidence(linear_rack(0)).has_value());
    RNF_REQUIRE_VALUE(activated, client.set_lifecycle(LifecycleState::kActive));
    RNF_CHECK(activated.code == StatusCode::kOk);

    AcquireRequest acquire;
    acquire.request.hi = 9;
    acquire.request.lo = 9;
    acquire.principal = PrincipalId{1};
    acquire.scope = ResourceRef{ResourceKind::kPort, 20};
    acquire.capacity = Capacity{50};
    acquire.ttl_ms = 60000;

    RNF_REQUIRE_VALUE(first, client.acquire(acquire));
    RNF_REQUIRE_VALUE(replay, client.acquire(acquire));
    RNF_CHECK(first.grant.id == replay.grant.id);
    RNF_CHECK_EQ(first.grant.fence, replay.grant.fence);

    RNF_REQUIRE_VALUE(lookup, client.lookup(acquire.request));
    RNF_CHECK(lookup.remembered);
    RNF_CHECK(lookup.outcome_code == StatusCode::kOk);
    RNF_CHECK(lookup.grant == first.grant.id);

    RequestId unknown;
    unknown.lo = 123456;
    RNF_REQUIRE_VALUE(missing, client.lookup(unknown));
    RNF_CHECK(!missing.remembered);
    RNF_CHECK(missing.code == StatusCode::kUnknown);
}

RNF_TEST(integration, lifecycle_refusals_are_typed) {
    Rig rig(rig_config());
    RNF_REQUIRE(rig.start());
    Client& client = rig.client();
    RNF_REQUIRE(client.submit_evidence(linear_rack(0)).has_value());

    // New authority is refused while the rack is still assembling.
    AcquireRequest acquire;
    acquire.request.lo = 5;
    acquire.principal = PrincipalId{1};
    acquire.scope = ResourceRef{ResourceKind::kDevice, 1};
    acquire.ttl_ms = 60000;
    RNF_REQUIRE_VALUE(early, client.acquire(acquire));
    RNF_CHECK(early.code == StatusCode::kLifecycleRefused);

    // Assembling to retired is legal; retired is terminal.
    RNF_REQUIRE_VALUE(retired, client.set_lifecycle(LifecycleState::kRetired));
    RNF_CHECK(retired.code == StatusCode::kOk);
    RNF_REQUIRE_VALUE(back, client.set_lifecycle(LifecycleState::kActive));
    RNF_CHECK(back.code == StatusCode::kInvalidLifecycleTransition);
    RNF_REQUIRE_VALUE(state, client.query_state());
    RNF_CHECK(state.lifecycle == LifecycleState::kRetired);
}

RNF_TEST(integration, maintenance_blocks_new_authority_over_tcp) {
    Rig rig(rig_config());
    RNF_REQUIRE(rig.start());
    Client& client = rig.client();
    RNF_REQUIRE(client.submit_evidence(linear_rack(0)).has_value());
    RNF_REQUIRE_VALUE(activated, client.set_lifecycle(LifecycleState::kActive));
    RNF_CHECK(activated.code == StatusCode::kOk);

    std::vector<EvidenceRecord> drain;
    drain.push_back(maintenance_record(MaintenanceKind::kDrain, ResourceKind::kDevice, 2, "work",
                                       0, kOperatorSource, 500));
    RNF_REQUIRE_VALUE(ack, client.submit_evidence(drain));
    RNF_CHECK_EQ(ack.entries.size(), 1U);

    AcquireRequest acquire;
    acquire.request.lo = 6;
    acquire.principal = PrincipalId{1};
    acquire.scope = ResourceRef{ResourceKind::kDevice, 2};
    acquire.capacity = Capacity{10};
    acquire.ttl_ms = 60000;
    RNF_REQUIRE_VALUE(refused, client.acquire(acquire));
    RNF_CHECK(refused.code == StatusCode::kScopeNotEligible);

    RNF_REQUIRE_VALUE(resources, client.query_resources(ResourceQuery{}));
    bool found = false;
    for (const ResourceEntry& entry : resources.resources) {
        if (entry.ref.kind == ResourceKind::kDevice && entry.ref.id == 2) {
            found = true;
            RNF_CHECK(entry.maintenance);
            RNF_CHECK(!(entry.eligibility == Eligibility::kEligible));
        }
    }
    RNF_CHECK(found);
}

RNF_TEST(integration, a_store_backed_daemon_reopens_with_the_same_state) {
    const std::filesystem::path directory = make_temp_directory("integration-store");
    StateResponse first;
    {
        DaemonConfig config = rig_config();
        config.store_directory = directory;
        Rig rig(config);
        RNF_REQUIRE(rig.start());
        RNF_REQUIRE(rig.client().submit_evidence(linear_rack(0)).has_value());
        RNF_REQUIRE_VALUE(activated, rig.client().set_lifecycle(LifecycleState::kActive));
        RNF_CHECK(activated.code == StatusCode::kOk);
        RNF_REQUIRE_VALUE(state, rig.client().query_state());
        first = state;
        RNF_REQUIRE_VALUE(checkpoint, rig.client().checkpoint());
        RNF_CHECK(checkpoint.code == StatusCode::kOk);
    }
    {
        DaemonConfig config = rig_config();
        config.store_directory = directory;
        Rig rig(config);
        RNF_REQUIRE(rig.start());
        RNF_REQUIRE_VALUE(state, rig.client().query_state());
        // Compare every component so a mismatch says which one moved.
        RNF_CHECK(state.evidence_digest == first.evidence_digest);
        RNF_CHECK(state.member_set_digest == first.member_set_digest);
        RNF_CHECK_EQ(state.generation.value, first.generation.value);
        RNF_CHECK_EQ(state.epoch.value, first.epoch.value);
        RNF_CHECK(state.lifecycle == first.lifecycle);
        RNF_CHECK_EQ(state.devices, first.devices);
        RNF_CHECK_EQ(state.ports, first.ports);
        RNF_CHECK_EQ(state.links, first.links);
        RNF_CHECK_EQ(state.total_diagnostics, first.total_diagnostics);
        RNF_CHECK(state.capacity.total.units == first.capacity.total.units);
        RNF_CHECK(state.capacity.usable.units == first.capacity.usable.units);
        RNF_CHECK(state.capacity.closes());
        RNF_CHECK(state.snapshot_digest == first.snapshot_digest);
        RNF_CHECK(state.incarnation.value > first.incarnation.value);
        RNF_CHECK(!state.recovered_from_dirty_shutdown);
        RNF_CHECK(state.clean_shutdown);
        // Nothing was silently re-authorized: there were no grants to recover.
        RNF_CHECK_EQ(state.live_grants, 0U);
        RNF_CHECK_EQ(state.recovering_grants, 0U);
    }
}

RNF_TEST(integration, client_reports_a_closed_daemon_cleanly) {
    DaemonConfig config = rig_config();
    DaemonCore core(config);
    RNF_REQUIRE_OK(core.start());
    DaemonServer server(core, ServerConfig{});
    RNF_REQUIRE_OK(server.bind("127.0.0.1", 0));
    std::atomic<bool> stop{false};
    std::thread runner([&]() { (void)server.run(&stop); });

    ClientOptions options;
    options.connect_attempts = 40;
    Result<Client> connected = Client::connect(Endpoint{"127.0.0.1", server.port()}, options);
    RNF_REQUIRE(connected.has_value());
    Client client = std::move(*connected);
    RNF_REQUIRE(client.hello(1).has_value());

    stop.store(true, std::memory_order_release);
    runner.join();
    const Result<StateResponse> after = client.query_state();
    RNF_CHECK(!after.has_value());
    RNF_CHECK(after.error().code() == StatusCode::kTruncated ||
              after.error().code() == StatusCode::kIo);
    client.close();
    RNF_REQUIRE_OK(core.stop(1));
}

RNF_TEST(integration, connecting_to_a_dead_endpoint_fails_with_a_typed_status) {
    // Bind a listener to get a port the operating system considers free, then
    // close it, so the connect attempt is refused promptly and deterministically
    // instead of relying on an arbitrary "probably closed" port.
    std::uint16_t free_port = 0;
    {
        Result<TcpListener> listener = TcpListener::bind_loopback(0, 4);
        RNF_REQUIRE(listener.has_value());
        free_port = listener->port();
        listener->close();
    }
    RNF_REQUIRE(free_port != 0);

    ClientOptions options;
    options.connect_attempts = 2;
    options.connect_retry_ms = 10;
    const Result<Client> connected = Client::connect(Endpoint{"127.0.0.1", free_port}, options);
    RNF_CHECK(!connected.has_value());
    if (!connected.has_value()) {
        RNF_CHECK(connected.error().code() == StatusCode::kIo ||
                  connected.error().code() == StatusCode::kCancelled);
    }
    // A malformed endpoint is refused before any socket is created.
    RNF_REQUIRE_CODE(Client::connect(Endpoint{"not-an-address", 0}, options),
                     StatusCode::kInvalidArgument);
}
