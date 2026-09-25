// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Completed-work benchmarks. Every number printed here is measured on the
// machine that runs the binary, reported as the median of the requested number
// of repetitions, and printed with the input size so the figure can be
// reproduced. Nothing is extrapolated and nothing is simulated.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rnf/authority/registry.hpp"
#include "rnf/compose/composer.hpp"
#include "rnf/net/frame.hpp"
#include "rnf/net/protocol.hpp"
#include "rnf/persist/store.hpp"
#include "rnf/runtime/client.hpp"
#include "rnf/runtime/daemon.hpp"
#include "rnf/runtime/server.hpp"
#include "rnf/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
    std::string name;
    double median_ms = 0;
    std::uint64_t operations = 0;
    std::string unit;
};

std::vector<Measurement>& measurements() {
    static std::vector<Measurement> results;
    return results;
}

double median_of(std::vector<double> samples) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t middle = samples.size() / 2;
    if (samples.size() % 2 == 1) {
        return samples[middle];
    }
    return (samples[middle - 1] + samples[middle]) / 2.0;
}

/// Run a body the requested number of times and record the median wall time.
template <class Body>
void measure(const std::string& name, std::uint64_t operations, const std::string& unit,
             int repetitions, Body&& body) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(repetitions));
    for (int i = 0; i < repetitions; ++i) {
        const auto start = Clock::now();
        body();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
        samples.push_back(static_cast<double>(elapsed.count()) / 1.0e6);
    }
    Measurement measurement;
    measurement.name = name;
    measurement.median_ms = median_of(std::move(samples));
    measurement.operations = operations;
    measurement.unit = unit;
    measurements().push_back(std::move(measurement));
}

rnf::EvidenceRecord member_record(std::uint64_t device, std::uint64_t incarnation,
                                  std::uint64_t sequence) {
    rnf::MemberClaim claim;
    claim.rack = rnf::RackId(1);
    claim.device = rnf::DeviceId(device);
    claim.incarnation = incarnation;
    rnf::EvidenceRecord record;
    record.kind = rnf::EvidenceKind::kMember;
    record.durability = rnf::Durability::kSticky;
    record.provenance.source = rnf::SourceId(1);
    record.provenance.kind = rnf::SourceKind::kOperator;
    record.provenance.sequence = sequence;
    record.payload = claim;
    return record;
}

rnf::EvidenceRecord device_record(std::uint64_t device, std::uint64_t capacity,
                                  std::uint64_t sequence) {
    rnf::DeviceClaim claim;
    claim.device = rnf::DeviceId(device);
    claim.role = rnf::DeviceRole::kSwitch;
    claim.capacity = rnf::Capacity{capacity};
    claim.availability = rnf::Availability::kUp;
    rnf::EvidenceRecord record;
    record.kind = rnf::EvidenceKind::kDevice;
    record.durability = rnf::Durability::kSticky;
    record.provenance.source = rnf::SourceId(1);
    record.provenance.kind = rnf::SourceKind::kOperator;
    record.provenance.sequence = sequence;
    record.payload = claim;
    return record;
}

rnf::EvidenceRecord port_record(std::uint64_t port, std::uint64_t device, std::uint16_t index,
                                rnf::PortRole role, std::uint64_t capacity, std::uint64_t sequence) {
    rnf::PortClaim claim;
    claim.port = rnf::PortId(port);
    claim.device = rnf::DeviceId(device);
    claim.index = index;
    claim.role = role;
    claim.capacity = rnf::Capacity{capacity};
    claim.availability = rnf::Availability::kUp;
    claim.admin = rnf::AdminState::kEnabled;
    rnf::EvidenceRecord record;
    record.kind = rnf::EvidenceKind::kPort;
    record.durability = rnf::Durability::kSticky;
    record.provenance.source = rnf::SourceId(1);
    record.provenance.kind = rnf::SourceKind::kOperator;
    record.provenance.sequence = sequence;
    record.payload = claim;
    return record;
}

rnf::EvidenceRecord link_record(std::uint64_t link, std::uint64_t a, std::uint64_t b,
                                std::uint64_t capacity, std::uint64_t sequence) {
    rnf::LinkClaim claim;
    claim.link = rnf::LinkId(link);
    claim.port_a = rnf::PortId(a);
    claim.port_b = rnf::PortId(b);
    claim.kind = rnf::LinkKind::kPhysical;
    claim.capacity = rnf::Capacity{capacity};
    claim.availability = rnf::Availability::kUp;
    rnf::EvidenceRecord record;
    record.kind = rnf::EvidenceKind::kLink;
    record.durability = rnf::Durability::kSticky;
    record.provenance.source = rnf::SourceId(1);
    record.provenance.kind = rnf::SourceKind::kOperator;
    record.provenance.sequence = sequence;
    record.payload = claim;
    return record;
}

/// A leaf and spine rack with the requested switch counts.
std::vector<rnf::EvidenceRecord> build_rack(std::uint64_t spine_count, std::uint64_t leaf_count) {
    std::vector<rnf::EvidenceRecord> records;
    std::uint64_t sequence = 1;
    std::uint64_t next_port = 1;
    std::vector<std::uint64_t> spine_ports;

    auto add_device = [&](std::uint64_t device, std::uint64_t ports, rnf::PortRole port_role,
                          std::vector<std::uint64_t>& port_ids) {
        records.push_back(member_record(device, device * 7, sequence++));
        records.push_back(device_record(device, 6400, sequence++));
        for (std::uint64_t i = 0; i < ports; ++i) {
            const std::uint64_t id = next_port++;
            port_ids.push_back(id);
            records.push_back(
                port_record(id, device, static_cast<std::uint16_t>(i), port_role, 1000, sequence++));
        }
    };

    for (std::uint64_t s = 0; s < spine_count; ++s) {
        add_device(100 + s, leaf_count, rnf::PortRole::kFabric, spine_ports);
    }
    std::vector<std::uint64_t> leaf_uplinks;
    for (std::uint64_t l = 0; l < leaf_count; ++l) {
        std::vector<std::uint64_t> uplinks;
        add_device(200 + l, spine_count, rnf::PortRole::kUplink, uplinks);
        leaf_uplinks.insert(leaf_uplinks.end(), uplinks.begin(), uplinks.end());
    }
    std::uint64_t link = 1;
    for (std::uint64_t l = 0; l < leaf_count; ++l) {
        for (std::uint64_t s = 0; s < spine_count; ++s) {
            records.push_back(
                link_record(link++, spine_ports[s], leaf_uplinks[l], 1000, sequence++));
        }
    }
    return records;
}

rnf::Snapshot compose_records(const std::vector<rnf::EvidenceRecord>& records) {
    rnf::EvidenceLedger ledger;
    for (const rnf::EvidenceRecord& record : records) {
        (void)ledger.insert(record);
    }
    rnf::RackConfig config;
    config.rack = rnf::RackId(1);
    config.name = "bench";
    rnf::ComposeInput input;
    input.generation = rnf::TopologyGeneration{1};
    input.epoch = rnf::RackEpoch{1};
    input.lifecycle = rnf::LifecycleState::kActive;
    const rnf::Result<rnf::ComposeOutcome> outcome = rnf::compose(ledger, config, input);
    if (!outcome.has_value()) {
        std::abort();
    }
    return outcome->snapshot;
}

void print_report() {
    std::printf("%-44s %12s %14s %14s\n", "benchmark", "median ms", "operations", "per second");
    std::printf("%s\n", std::string(88, '-').c_str());
    for (const Measurement& measurement : measurements()) {
        const double per_second =
            measurement.median_ms > 0
                ? (static_cast<double>(measurement.operations) * 1000.0) / measurement.median_ms
                : 0.0;
        std::printf("%-44s %12.3f %14llu %14.0f\n", measurement.name.c_str(),
                    measurement.median_ms,
                    static_cast<unsigned long long>(measurement.operations), per_second);
    }
}

}  // namespace

int main(int argc, char** argv) {
    int repetitions = 5;
    if (argc > 1) {
        repetitions = std::atoi(argv[1]);
        if (repetitions <= 0) {
            repetitions = 5;
        }
    }
    rnf::ensure_socket_runtime_noexcept();
    std::printf("Rack Network Fabric %s benchmarks, median of %d repetitions\n\n",
                std::string(rnf::kVersionString).c_str(), repetitions);

    {
        const std::vector<std::uint8_t> payload(64U * 1024U, 0x5A);
        measure("blake2s-256 over 64 KiB", 65536, "bytes", repetitions, [&]() {
            const rnf::Digest digest =
                rnf::Blake2s256::hash(rnf::ByteSpan(payload.data(), payload.size()));
            if (digest.is_zero()) {
                std::abort();
            }
        });
        measure("crc32c over 64 KiB", 65536, "bytes", repetitions, [&]() {
            if (rnf::crc32c(rnf::ByteSpan(payload.data(), payload.size())) == 0x12345678U) {
                std::abort();
            }
        });
    }
    {
        const rnf::EvidenceRecord record = device_record(9, 1000, 1);
        rnf::ByteWriter writer(4096);
        (void)rnf::encode(record, writer);
        const std::vector<std::uint8_t> bytes = writer.data();
        measure("evidence record decode", 200000, "records", repetitions, [&]() {
            for (int i = 0; i < 200000; ++i) {
                rnf::ByteReader reader(rnf::ByteSpan(bytes.data(), bytes.size()));
                const rnf::Result<rnf::EvidenceRecord> decoded = rnf::decode_evidence(reader);
                if (!decoded.has_value()) {
                    std::abort();
                }
            }
        });
        measure("evidence record digest", 200000, "records", repetitions, [&]() {
            for (int i = 0; i < 200000; ++i) {
                if (rnf::evidence_digest(record).is_zero()) {
                    std::abort();
                }
            }
        });
    }

    const std::vector<std::pair<std::uint64_t, std::uint64_t>> shapes = {
        {2, 4}, {4, 16}, {8, 32}};
    for (const std::pair<std::uint64_t, std::uint64_t>& shape : shapes) {
        const std::vector<rnf::EvidenceRecord> records = build_rack(shape.first, shape.second);
        const std::string name = "compose " + std::to_string(shape.first) + "x" +
                                 std::to_string(shape.second) + " (" +
                                 std::to_string(records.size()) + " records)";
        measure(name, 1, "snapshot", repetitions, [&]() {
            const rnf::Snapshot snapshot = compose_records(records);
            if (!snapshot.valid()) {
                std::abort();
            }
        });
    }

    {
        const std::vector<rnf::EvidenceRecord> records = build_rack(4, 16);
        measure("ledger insert", records.size(), "records", repetitions, [&]() {
            rnf::EvidenceLedger ledger;
            for (const rnf::EvidenceRecord& record : records) {
                (void)ledger.insert(record);
            }
        });
    }

    {
        const std::vector<rnf::EvidenceRecord> records = build_rack(2, 4);
        const rnf::Snapshot snapshot = compose_records(records);
        if (!snapshot.valid()) {
            std::abort();
        }
        measure("grant acquire + release", 20000, "grants", repetitions, [&]() {
            rnf::GrantRegistry registry;
            registry.set_incarnation(rnf::ControllerIncarnation{1});
            for (std::uint64_t i = 0; i < 20000; ++i) {
                rnf::GrantRequest request;
                request.request.lo = i + 1;
                request.principal = rnf::PrincipalId{1};
                request.scope = rnf::ResourceRef{rnf::ResourceKind::kDevice, 100};
                request.capacity = rnf::Capacity{1};
                request.ttl_ms = 60000;
                const rnf::Result<rnf::Grant> granted =
                    registry.acquire(snapshot, request, rnf::ControllerIncarnation{1},
                                     1700000000000ULL + i, true);
                if (!granted.has_value()) {
                    std::abort();
                }
                if (!registry.release(granted->token(), 1700000000000ULL + i).ok()) {
                    std::abort();
                }
            }
        });
    }

    {
        const std::filesystem::path directory =
            std::filesystem::temp_directory_path() / "rnf-bench-store";
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
        rnf::StoreOptions options;
        options.fsync_on_append = true;
        {
            rnf::Result<std::unique_ptr<rnf::Store>> opened = rnf::Store::open(directory, options);
            if (!opened.has_value()) {
                std::abort();
            }
            std::unique_ptr<rnf::Store> store = std::move(*opened);
            measure("durable append (fsync per record)", 500, "records", repetitions, [&]() {
                for (std::uint64_t i = 0; i < 500; ++i) {
                    rnf::StoreRecord record;
                    record.type = rnf::RecordType::kEvidence;
                    record.payload = device_record(i + 1, 100, 1);
                    if (!store->append(record).ok()) {
                        std::abort();
                    }
                }
            });
        }
        options.fsync_on_append = false;
        {
            rnf::Result<std::unique_ptr<rnf::Store>> opened = rnf::Store::open(directory, options);
            if (opened.has_value()) {
                std::unique_ptr<rnf::Store> store = std::move(*opened);
                measure("buffered append (no fsync)", 20000, "records", repetitions, [&]() {
                    for (std::uint64_t i = 0; i < 20000; ++i) {
                        rnf::StoreRecord record;
                        record.type = rnf::RecordType::kEvidence;
                        record.payload = device_record(i + 1, 100, 1);
                        if (!store->append(record).ok()) {
                            std::abort();
                        }
                    }
                });
            }
        }
        std::filesystem::remove_all(directory, ec);
    }

    {
        rnf::DaemonConfig config;
        config.rack.rack = rnf::RackId(1);
        config.rack.name = "bench";
        config.auto_compose = true;
        rnf::DaemonCore core(config);
        if (!core.start().ok()) {
            std::abort();
        }
        rnf::DaemonServer server(core, rnf::ServerConfig{});
        if (!server.bind("127.0.0.1", 0).ok()) {
            std::abort();
        }
        std::atomic<bool> stop{false};
        std::thread runner([&]() { (void)server.run(&stop); });
        rnf::ClientOptions client_options;
        client_options.connect_attempts = 50;
        rnf::Result<rnf::Client> connected =
            rnf::Client::connect(rnf::Endpoint{"127.0.0.1", server.port()}, client_options);
        if (!connected.has_value()) {
            std::abort();
        }
        rnf::Client client = std::move(*connected);
        if (!client.hello(1).has_value()) {
            std::abort();
        }
        const std::vector<rnf::EvidenceRecord> records = build_rack(2, 4);
        if (!client.submit_evidence(records).has_value()) {
            std::abort();
        }
        if (!client.set_lifecycle(rnf::LifecycleState::kActive).has_value()) {
            std::abort();
        }
        measure("daemon round trip: query_state", 2000, "requests", repetitions, [&]() {
            for (int i = 0; i < 2000; ++i) {
                if (!client.query_state().has_value()) {
                    std::abort();
                }
            }
        });
        measure("daemon round trip: acquire + release", 2000, "requests", repetitions, [&]() {
            for (std::uint64_t i = 0; i < 2000; ++i) {
                rnf::AcquireRequest request;
                request.request.hi = 0xBEEF;
                request.request.lo = i + 1;
                request.principal = rnf::PrincipalId{1};
                request.scope = rnf::ResourceRef{rnf::ResourceKind::kDevice, 100};
                request.capacity = rnf::Capacity{1};
                request.ttl_ms = 60000;
                const rnf::Result<rnf::GrantResponse> granted = client.acquire(request);
                if (!granted.has_value() || granted->code != rnf::StatusCode::kOk) {
                    std::abort();
                }
                rnf::LeaseToken token;
                token.grant = granted->grant.id;
                token.fence = granted->grant.fence;
                token.incarnation = granted->grant.incarnation;
                token.scope_basis = granted->grant.basis;
                if (!client.release(token).has_value()) {
                    std::abort();
                }
            }
        });
        client.close();
        stop.store(true, std::memory_order_release);
        server.request_stop();
        runner.join();
        (void)core.stop(1);
    }

    std::printf("\n");
    print_report();
    return 0;
}
