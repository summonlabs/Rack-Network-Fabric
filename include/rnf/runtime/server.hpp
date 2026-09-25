// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The loopback TCP front end. One connection is handled by one worker thread;
// every command is a synchronous call into DaemonCore, which owns the only
// mutex in the process.

#ifndef RNF_RUNTIME_SERVER_HPP
#define RNF_RUNTIME_SERVER_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "rnf/net/socket.hpp"
#include "rnf/runtime/daemon.hpp"

namespace rnf {

struct ServerConfig {
    std::uint32_t max_frame_payload = kDefaultMaxFramePayload;
    std::size_t max_connections = 32;
    /// Idle timeout for a connection that stops sending. A killed peer closes
    /// its socket, so this only bounds half-open connections.
    std::uint32_t idle_timeout_ms = 300000;
    /// How often the lease expiry ticker runs.
    std::uint32_t tick_interval_ms = 200;
};

/// Threading: run() blocks on the calling thread. request_stop() may be called
/// from any thread. The server never holds the daemon mutex while it waits on a
/// socket, and never holds its own worker list lock while joining.
class DaemonServer {
public:
    DaemonServer(DaemonCore& core, ServerConfig config);
    ~DaemonServer();
    DaemonServer(const DaemonServer&) = delete;
    DaemonServer& operator=(const DaemonServer&) = delete;

    /// Bind the listening socket. Must be called before run().
    [[nodiscard]] Status bind(const std::string& host, std::uint16_t port);

    /// Serve until request_stop() is called or an external stop flag is set.
    [[nodiscard]] Status run(std::atomic<bool>* external_stop = nullptr);

    void request_stop() noexcept { stop_.store(true, std::memory_order_release); }
    [[nodiscard]] std::uint16_t port() const noexcept { return listener_.port(); }

private:
    void handle_connection(TcpStream stream, std::shared_ptr<std::atomic<bool>> finished);
    void reap_workers(bool join_all);
    [[nodiscard]] Status send_error(TcpStream& stream, std::uint64_t correlation, StatusCode code,
                                    const std::string& detail);
    [[nodiscard]] Status dispatch(TcpStream& stream, MessageType type, std::uint64_t correlation,
                                  ByteSpan payload);

    DaemonCore& core_;
    ServerConfig config_;
    TcpListener listener_;
    std::atomic<bool> stop_{false};
    std::thread ticker_;
    std::atomic<bool> ticker_stop_{false};

    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<Worker> workers_;
    std::mutex workers_mutex_;
    std::atomic<std::size_t> active_connections_{0};
};

}  // namespace rnf

#endif  // RNF_RUNTIME_SERVER_HPP
