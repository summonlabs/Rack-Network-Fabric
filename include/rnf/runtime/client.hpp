// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Blocking client for rnfd. One request is in flight at a time on a connection;
// the client is safe to use from a single thread.

#ifndef RNF_RUNTIME_CLIENT_HPP
#define RNF_RUNTIME_CLIENT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "rnf/net/protocol.hpp"
#include "rnf/net/socket.hpp"

namespace rnf {

struct ClientOptions {
    std::uint32_t max_frame_payload = kDefaultMaxFramePayload;
    std::uint32_t connect_attempts = 20;
    std::uint32_t connect_retry_ms = 50;
    std::uint32_t io_timeout_ms = 300000;
};

class Client {
public:
    Client() = default;

    [[nodiscard]] static Result<Client> connect(const Endpoint& endpoint,
                                                const ClientOptions& options = {});

    [[nodiscard]] bool valid() const noexcept { return stream_.valid(); }
    void close() noexcept { stream_.close(); }

    [[nodiscard]] Result<HelloResponse> hello(std::uint32_t client_id);
    [[nodiscard]] Result<EvidenceAckResponse> submit_evidence(
        const std::vector<EvidenceRecord>& records);
    [[nodiscard]] Result<ComposeResponse> compose(const TopologyGeneration& generation);
    [[nodiscard]] Result<GrantResponse> acquire(const AcquireRequest& request);
    [[nodiscard]] Result<GrantResponse> renew(const LeaseToken& token, std::uint64_t ttl_ms);
    [[nodiscard]] Result<AckResponse> release(const LeaseToken& token);
    [[nodiscard]] Result<GrantResponse> validate(const LeaseToken& token);
    [[nodiscard]] Result<LookupResponse> lookup(const RequestId& request);
    [[nodiscard]] Result<StateResponse> query_state();
    [[nodiscard]] Result<ResourcesResponse> query_resources(const ResourceQuery& query);
    [[nodiscard]] Result<PathsResponse> query_paths();
    [[nodiscard]] Result<GrantsResponse> query_grants();
    [[nodiscard]] Result<LifecycleResponse> set_lifecycle(LifecycleState target);
    [[nodiscard]] Result<AckResponse> checkpoint();
    [[nodiscard]] Result<StatsResponse> stats();
    [[nodiscard]] Result<AckResponse> shutdown();

    /// Correlation identifier of the most recent exchange, for diagnostics.
    [[nodiscard]] std::uint64_t last_correlation() const noexcept { return last_correlation_; }

private:
    [[nodiscard]] Status send_raw(MessageType type, ByteSpan payload);
    [[nodiscard]] Result<std::vector<std::uint8_t>> receive_raw(MessageType expected);

    TcpStream stream_;
    ClientOptions options_;
    std::uint64_t next_correlation_ = 1;
    std::uint64_t last_correlation_ = 0;
};

}  // namespace rnf

#endif  // RNF_RUNTIME_CLIENT_HPP
