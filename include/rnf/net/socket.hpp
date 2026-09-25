// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Minimal blocking TCP transport over loopback. Deliberately small: the
// runtime's distributed proof needs real OS sockets between real processes, not
// a general purpose networking stack.

#ifndef RNF_NET_SOCKET_HPP
#define RNF_NET_SOCKET_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "rnf/core/bytes.hpp"
#include "rnf/core/status.hpp"

namespace rnf {

/// One-time platform socket initialisation. Idempotent and thread safe.
[[nodiscard]] Status ensure_socket_runtime();

/// Initialise the socket runtime and ignore the result. Safe to call from a
/// static initialiser.
void ensure_socket_runtime_noexcept() noexcept;

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
    [[nodiscard]] std::string to_text() const;
};

/// A connected stream socket.
///
/// Threading: one thread may read while another writes. Concurrent reads or
/// concurrent writes on the same stream are not supported.
class TcpStream {
public:
    TcpStream() = default;
    ~TcpStream();
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& other) noexcept;
    TcpStream& operator=(TcpStream&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    void close() noexcept;

    /// Write the whole buffer. Returns kIo on a short or failed write.
    [[nodiscard]] Status send_all(ByteSpan data);
    /// Read up to data.size() bytes. Returns 0 on an orderly shutdown.
    [[nodiscard]] Result<std::size_t> recv_some(ByteSpan data);
    /// Read exactly data.size() bytes or fail with kTruncated.
    [[nodiscard]] Status recv_exact(ByteSpan data);
    /// Read exactly data.size() bytes, but return kCancelled as soon as the
    /// stop flag is set. This is what lets a server shut down without waiting
    /// for an idle peer: the waiter is never blocked inside a socket call that
    /// another thread would have to close underneath it.
    [[nodiscard]] Status recv_exact_interruptible(ByteSpan data, const std::atomic<bool>& stop,
                                                  std::uint32_t idle_timeout_ms);
    /// Discard up to the requested number of bytes; used to resynchronise the
    /// stream after a rejected frame.
    void discard(std::size_t count) noexcept;

    [[nodiscard]] Status set_read_timeout_ms(std::uint32_t milliseconds);
    [[nodiscard]] Status set_write_timeout_ms(std::uint32_t milliseconds);
    [[nodiscard]] int native_handle() const noexcept { return handle_; }
    void adopt(int handle) noexcept { handle_ = handle; }

private:
    int handle_ = -1;
};

/// A listening socket bound to a loopback address.
class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    /// Bind to 127.0.0.1 on the given port. Port 0 asks the operating system
    /// for a free port; the chosen port is reported by port().
    [[nodiscard]] static Result<TcpListener> bind_loopback(std::uint16_t port,
                                                           int backlog = 16);

    /// Accept one connection. The predicate is polled while waiting, and when
    /// it returns true accept() gives up with kCancelled. Passing the predicate
    /// rather than a single flag is what lets a server combine its own stop
    /// flag with an external one; a loop that only re-checked between accepts
    /// would block forever when nothing ever connects.
    [[nodiscard]] Result<TcpStream> accept(const std::function<bool()>& should_stop);

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ >= 0; }
    void close() noexcept;

private:
    int handle_ = -1;
    std::uint16_t port_ = 0;
};

/// Connect to a loopback endpoint. Retries briefly because a freshly started
/// listener may not be accepting yet.
[[nodiscard]] Result<TcpStream> connect_loopback(const Endpoint& endpoint,
                                                 std::uint32_t attempts = 1,
                                                 std::uint32_t delay_ms = 25);

/// Total bytes and frames moved by this process, for diagnostics only.
struct SocketCounters {
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t frames_sent = 0;
    std::uint64_t frames_received = 0;
    std::uint64_t frames_rejected = 0;
};

[[nodiscard]] SocketCounters socket_counters() noexcept;
void add_bytes_sent(std::uint64_t count) noexcept;
void add_bytes_received(std::uint64_t count) noexcept;
void add_frame_sent() noexcept;
void add_frame_received() noexcept;
void add_frame_rejected() noexcept;

}  // namespace rnf

#endif  // RNF_NET_SOCKET_HPP
