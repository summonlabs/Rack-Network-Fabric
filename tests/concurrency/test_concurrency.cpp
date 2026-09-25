// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Concurrency and ownership tests. These exercise the exact hazards the design
// audit calls out: re-entrant locks, callbacks under locks, joining workers
// while holding state they need, shutdown races and stale completions.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "rnf/persist/store.hpp"
#include "rnf/runtime/client.hpp"
#include "rnf/runtime/daemon.hpp"
#include "rnf/runtime/server.hpp"
#include "tests/support/fixtures.hpp"
#include "tests/support/harness.hpp"

using namespace rnf;
using namespace rnf::test;

namespace {

DaemonConfig concurrent_config() {
    DaemonConfig config;
    config.rack = linear_rack_config();
    config.rack.name = "concurrency";
    config.auto_compose = true;
    config.max_connections = 16;
    return config;
}

}  // namespace

RNF_TEST(concurrency, many_writers_and_readers_on_one_core) {
    DaemonCore core(concurrent_config());
    RNF_REQUIRE_OK(core.start());

    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::vector<std::thread> workers;

    // Writers: each thread submits its own disjoint slice of evidence.
    for (std::uint64_t writer = 0; writer < 4; ++writer) {
        workers.emplace_back([&, writer]() {
            for (std::uint64_t round = 0; round < 20; ++round) {
                SubmitEvidenceRequest request;
                const std::uint64_t device = 1000 + writer * 100 + round;
                request.records.push_back(member_record(device, device, 0, kOperatorSource,
                                                        writer * 1000 + round + 1));
                request.records.push_back(device_record(device, DeviceRole::kHost, Capacity{10},
                                                        Availability::kUp, 0, kOperatorSource,
                                                        writer * 1000 + round + 1));
                const Result<EvidenceAckResponse> ack = core.submit_evidence(request);
                if (!ack.has_value()) {
                    errors.fetch_add(1);
                }
            }
        });
    }
    // Readers: continuously observe state while the writers run.
    for (std::uint64_t reader = 0; reader < 4; ++reader) {
        workers.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                const Result<StateResponse> state = core.query_state();
                if (!state.has_value()) {
                    errors.fetch_add(1);
                    continue;
                }
                if (!state->capacity.closes()) {
                    errors.fetch_add(1);
                }
                const Result<ResourcesResponse> resources =
                    core.query_resources(ResourceQuery{});
                if (!resources.has_value()) {
                    errors.fetch_add(1);
                }
            }
        });
    }
    // A ticker thread exercises the same expiry path the server uses.
    workers.emplace_back([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            (void)core.expire_due(1000);
            std::this_thread::yield();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }
    RNF_CHECK_EQ(errors.load(), 0);
    RNF_REQUIRE_VALUE(state, core.query_state());
    RNF_CHECK(state.capacity.closes());
    RNF_REQUIRE_OK(core.stop(1));
    // Stopping twice must be safe and must not double count anything.
    RNF_REQUIRE_OK(core.stop(2));
}

RNF_TEST(concurrency, grant_lifecycle_under_contention) {
    DaemonCore core(concurrent_config());
    RNF_REQUIRE_OK(core.start());
    SubmitEvidenceRequest seed;
    seed.records = linear_rack(0);
    RNF_REQUIRE_VALUE(ack, core.submit_evidence(seed));
    RNF_CHECK_EQ(ack.entries.size(), seed.records.size());
    RNF_REQUIRE_VALUE(active, core.set_lifecycle(LifecycleRequest{LifecycleState::kActive}));
    RNF_CHECK(active.code == StatusCode::kOk);

    std::atomic<int> unexpected{0};
    std::atomic<int> acquired{0};
    std::vector<std::thread> workers;
    for (std::uint64_t worker = 0; worker < 6; ++worker) {
        workers.emplace_back([&, worker]() {
            for (int iteration = 0; iteration < 60; ++iteration) {
                AcquireRequest request;
                request.request.hi = worker + 1;
                request.request.lo = static_cast<std::uint64_t>(iteration) + 1;
                request.principal = PrincipalId{worker + 1};
                request.scope = ResourceRef{ResourceKind::kDevice,
                                            static_cast<std::uint64_t>(1 + (iteration % 2))};
                request.mode = (iteration % 7 == 0) ? GrantMode::kExclusive : GrantMode::kShared;
                request.capacity = Capacity{2};
                request.ttl_ms = 60000;
                const Result<GrantResponse> granted = core.acquire(request);
                if (!granted.has_value()) {
                    unexpected.fetch_add(1);
                    continue;
                }
                if (granted->code != StatusCode::kOk) {
                    // Only the documented refusals are acceptable here.
                    if (granted->code != StatusCode::kExclusiveConflict &&
                        granted->code != StatusCode::kCapacityExhausted &&
                        granted->code != StatusCode::kScopeNotEligible) {
                        unexpected.fetch_add(1);
                    }
                    continue;
                }
                acquired.fetch_add(1);
                TokenMessage token;
                token.token.grant = granted->grant.id;
                token.token.fence = granted->grant.fence;
                token.token.incarnation = granted->grant.incarnation;
                token.token.scope_basis = granted->grant.basis;
                const Result<GrantResponse> valid = core.validate(token);
                if (!valid.has_value() || valid->code != StatusCode::kOk) {
                    unexpected.fetch_add(1);
                }
                const Result<AckResponse> released = core.release(token);
                if (!released.has_value() || released->code != StatusCode::kOk) {
                    unexpected.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    RNF_CHECK_EQ(unexpected.load(), 0);
    RNF_CHECK(acquired.load() > 0);
    RNF_REQUIRE_VALUE(state, core.query_state());
    RNF_CHECK_EQ(state.live_grants, 0U);
    RNF_CHECK_EQ(state.capacity.committed_grants.units, 0U);
    RNF_REQUIRE_OK(core.stop(1));
}

RNF_TEST(concurrency, tcp_clients_are_served_concurrently) {
    DaemonCore core(concurrent_config());
    RNF_REQUIRE_OK(core.start());
    DaemonServer server(core, ServerConfig{});
    RNF_REQUIRE_OK(server.bind("127.0.0.1", 0));
    std::atomic<bool> stop{false};
    std::thread runner([&]() { RNF_CHECK(server.run(&stop).ok()); });

    const Endpoint endpoint{"127.0.0.1", server.port()};
    std::atomic<int> errors{0};
    std::atomic<int> served{0};
    std::vector<std::thread> clients;
    for (std::uint64_t index = 0; index < 8; ++index) {
        clients.emplace_back([&, index]() {
            ClientOptions options;
            options.connect_attempts = 40;
            options.connect_retry_ms = 25;
            Result<Client> connected = Client::connect(endpoint, options);
            if (!connected.has_value()) {
                errors.fetch_add(1);
                return;
            }
            Client client = std::move(*connected);
            const Result<HelloResponse> hello = client.hello(static_cast<std::uint32_t>(index));
            if (!hello.has_value() || !hello->accepted) {
                errors.fetch_add(1);
                return;
            }
            for (int round = 0; round < 10; ++round) {
                SubmitEvidenceRequest request;
                const std::uint64_t device = 5000 + index * 100 + static_cast<std::uint64_t>(round);
                request.records.push_back(member_record(device, device, 0, kOperatorSource,
                                                        index * 100 + static_cast<std::uint64_t>(round) + 1));
                const Result<EvidenceAckResponse> ack = client.submit_evidence(request.records);
                if (!ack.has_value() || ack->entries.size() != 1U) {
                    errors.fetch_add(1);
                    return;
                }
                const Result<StateResponse> state = client.query_state();
                if (!state.has_value()) {
                    errors.fetch_add(1);
                    return;
                }
                served.fetch_add(1);
            }
            client.close();
        });
    }
    for (std::thread& client : clients) {
        client.join();
    }
    RNF_CHECK_EQ(errors.load(), 0);
    RNF_CHECK_EQ(served.load(), 80);

    stop.store(true, std::memory_order_release);
    runner.join();
    RNF_REQUIRE_OK(core.stop(1));
}

RNF_TEST(concurrency, shutdown_with_a_client_mid_session) {
    DaemonCore core(concurrent_config());
    RNF_REQUIRE_OK(core.start());
    DaemonServer server(core, ServerConfig{});
    RNF_REQUIRE_OK(server.bind("127.0.0.1", 0));
    std::atomic<bool> stop{false};
    std::thread runner([&]() { RNF_CHECK(server.run(&stop).ok()); });

    ClientOptions options;
    options.connect_attempts = 40;
    Result<Client> connected = Client::connect(Endpoint{"127.0.0.1", server.port()}, options);
    RNF_REQUIRE(connected.has_value());
    Client client = std::move(*connected);
    RNF_REQUIRE(client.hello(1).has_value());

    // Ask the daemon to stop while this very connection is open.
    const Result<AckResponse> acknowledged = client.shutdown();
    RNF_REQUIRE(acknowledged.has_value());
    runner.join();
    // The stream is now closed; the next request must fail cleanly.
    const Result<StateResponse> after = client.query_state();
    RNF_CHECK(!after.has_value());
    client.close();
    RNF_REQUIRE_OK(core.stop(1));
}

RNF_TEST(concurrency, connection_bound_is_enforced_without_hanging) {
    DaemonConfig config = concurrent_config();
    config.max_connections = 2;
    DaemonCore core(config);
    RNF_REQUIRE_OK(core.start());
    ServerConfig server_config;
    server_config.max_connections = 2;
    DaemonServer server(core, server_config);
    RNF_REQUIRE_OK(server.bind("127.0.0.1", 0));
    std::atomic<bool> stop{false};
    std::thread runner([&]() { RNF_CHECK(server.run(&stop).ok()); });

    std::vector<Client> clients;
    for (int index = 0; index < 2; ++index) {
        ClientOptions options;
        options.connect_attempts = 40;
        Result<Client> connected =
            Client::connect(Endpoint{"127.0.0.1", server.port()}, options);
        RNF_REQUIRE(connected.has_value());
        RNF_REQUIRE(connected->hello(1).has_value());
        clients.push_back(std::move(*connected));
    }
    // The third connection is accepted and dropped without blocking the others.
    ClientOptions options;
    options.connect_attempts = 40;
    Result<Client> rejected = Client::connect(Endpoint{"127.0.0.1", server.port()}, options);
    if (rejected.has_value()) {
        (void)rejected->hello(1);
        rejected->close();
    }
    for (Client& client : clients) {
        RNF_REQUIRE(client.query_state().has_value());
    }
    for (Client& client : clients) {
        client.close();
    }
    stop.store(true, std::memory_order_release);
    runner.join();
    RNF_REQUIRE_VALUE(stats, core.stats());
    RNF_CHECK(stats.connections_rejected >= 1U);
    RNF_REQUIRE_OK(core.stop(1));
}

RNF_TEST(concurrency, store_open_close_cycles_preserve_every_record) {
    const std::filesystem::path directory = make_temp_directory("concurrency-store");
    for (int cycle = 0; cycle < 5; ++cycle) {
        Result<std::unique_ptr<Store>> opened = Store::open(directory, StoreOptions{});
        RNF_REQUIRE(opened.has_value());
        std::unique_ptr<Store> store = std::move(*opened);
        for (std::uint64_t round = 0; round < 30; ++round) {
            StoreRecord record;
            record.type = RecordType::kEvidence;
            record.payload = device_record(static_cast<std::uint64_t>(cycle) * 100 + round + 1,
                                           DeviceRole::kHost, Capacity{1}, Availability::kUp, 0,
                                           kOperatorSource, round + 1);
            RNF_REQUIRE_OK(store->append(record));
        }
        RNF_REQUIRE_OK(store->mark_clean_shutdown(1));
        store.reset();
    }
    RNF_REQUIRE_VALUE(store, Store::open(directory, StoreOptions{}));
    RNF_CHECK(store->recovery().clean_shutdown);
    RNF_CHECK_EQ(store->recovered().records.size(), 150U);
}

RNF_TEST(concurrency, snapshot_handles_are_safe_to_read_concurrently) {
    // A snapshot is immutable once published, so any number of threads may read
    // one while the composer builds the next generation.
    EvidenceLedger ledger;
    fill(ledger, linear_rack(0));
    Result<ComposeOutcome> outcome = compose(ledger, linear_rack_config(), compose_input(0));
    RNF_REQUIRE(outcome.has_value());
    const Snapshot published = outcome->snapshot;

    std::atomic<int> errors{0};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 6; ++reader) {
        readers.emplace_back([&]() {
            for (int round = 0; round < 200; ++round) {
                if (published->devices.size() != 2U) {
                    errors.fetch_add(1);
                }
                if (!published->capacity.closes()) {
                    errors.fetch_add(1);
                }
                if (snapshot_digest(published.data()) != published.digest()) {
                    errors.fetch_add(1);
                }
                const ResourceState* state =
                    published.find(ResourceRef{ResourceKind::kDevice, 1});
                if (state == nullptr || state->eligibility != Eligibility::kEligible) {
                    errors.fetch_add(1);
                }
            }
        });
    }
    // Meanwhile the producing thread keeps composing new generations.
    std::thread producer([&]() {
        for (std::uint64_t generation = 1; generation <= 20; ++generation) {
            EvidenceLedger next;
            fill(next, linear_rack(generation));
            Result<ComposeOutcome> built =
                compose(next, linear_rack_config(), compose_input(generation));
            if (!built.has_value()) {
                errors.fetch_add(1);
            }
        }
    });
    producer.join();
    for (std::thread& reader : readers) {
        reader.join();
    }
    RNF_CHECK_EQ(errors.load(), 0);
}
