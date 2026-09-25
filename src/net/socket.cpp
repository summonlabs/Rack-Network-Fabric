// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/net/socket.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rnf {
namespace {

#ifdef _WIN32
using native_socket = SOCKET;
constexpr native_socket kInvalidSocket = INVALID_SOCKET;
#else
using native_socket = int;
constexpr native_socket kInvalidSocket = -1;
#endif

std::atomic<bool> g_socket_ready{false};
std::atomic<std::uint64_t> g_bytes_sent{0};
std::atomic<std::uint64_t> g_bytes_received{0};
std::atomic<std::uint64_t> g_frames_sent{0};
std::atomic<std::uint64_t> g_frames_received{0};
std::atomic<std::uint64_t> g_frames_rejected{0};

native_socket to_native(int handle) noexcept {
    return static_cast<native_socket>(handle);
}

int from_native(native_socket socket) noexcept {
    return static_cast<int>(socket);
}

void close_native(int handle) noexcept {
    if (handle < 0) {
        return;
    }
#ifdef _WIN32
    ::closesocket(to_native(handle));
#else
    ::close(handle);
#endif
}

bool would_block() noexcept {
#ifdef _WIN32
    const int error = ::WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT || error == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

}  // namespace

Status ensure_socket_runtime() {
    static std::atomic<bool> initialised{false};
    static std::atomic<int> result{0};
    if (initialised.load(std::memory_order_acquire)) {
        return result.load(std::memory_order_relaxed) == 0
                   ? Status{}
                   : Status(StatusCode::kIo, "the socket runtime failed to initialise");
    }
#ifdef _WIN32
    WSADATA data{};
    const int status = ::WSAStartup(MAKEWORD(2, 2), &data);
    result.store(status == 0 ? 0 : status, std::memory_order_relaxed);
#else
    result.store(0, std::memory_order_relaxed);
#endif
    initialised.store(true, std::memory_order_release);
    g_socket_ready.store(true, std::memory_order_release);
    return result.load(std::memory_order_relaxed) == 0
               ? Status{}
               : Status(StatusCode::kIo, "the socket runtime failed to initialise");
}

void ensure_socket_runtime_noexcept() noexcept {
    (void)ensure_socket_runtime();
}

std::string Endpoint::to_text() const {
    return host + ":" + std::to_string(port);
}

void add_bytes_sent(std::uint64_t count) noexcept {
    g_bytes_sent.fetch_add(count, std::memory_order_relaxed);
}
void add_bytes_received(std::uint64_t count) noexcept {
    g_bytes_received.fetch_add(count, std::memory_order_relaxed);
}
void add_frame_sent() noexcept {
    g_frames_sent.fetch_add(1, std::memory_order_relaxed);
}
void add_frame_received() noexcept {
    g_frames_received.fetch_add(1, std::memory_order_relaxed);
}
void add_frame_rejected() noexcept {
    g_frames_rejected.fetch_add(1, std::memory_order_relaxed);
}

SocketCounters socket_counters() noexcept {
    SocketCounters counters;
    counters.bytes_sent = g_bytes_sent.load(std::memory_order_relaxed);
    counters.bytes_received = g_bytes_received.load(std::memory_order_relaxed);
    counters.frames_sent = g_frames_sent.load(std::memory_order_relaxed);
    counters.frames_received = g_frames_received.load(std::memory_order_relaxed);
    counters.frames_rejected = g_frames_rejected.load(std::memory_order_relaxed);
    return counters;
}

// ---------------------------------------------------------------------------
// TcpStream
// ---------------------------------------------------------------------------

TcpStream::~TcpStream() {
    close();
}

TcpStream::TcpStream(TcpStream&& other) noexcept : handle_(other.handle_) {
    other.handle_ = -1;
}

TcpStream& TcpStream::operator=(TcpStream&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = -1;
    }
    return *this;
}

bool TcpStream::valid() const noexcept {
    return handle_ >= 0;
}

void TcpStream::close() noexcept {
    close_native(handle_);
    handle_ = -1;
}

Status TcpStream::send_all(ByteSpan data) {
    if (!valid()) {
        return Status(StatusCode::kClosed, "stream is not connected");
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t chunk = data.size() - offset;
        const int request = static_cast<int>(chunk > 0x40000000U ? 0x40000000U : chunk);
        const int written = ::send(to_native(handle_),
                                   reinterpret_cast<const char*>(data.data() + offset), request, 0);
        if (written <= 0) {
            if (would_block()) {
                return Status(StatusCode::kIo, "send timed out");
            }
            return Status(StatusCode::kIo, "send failed");
        }
        offset += static_cast<std::size_t>(written);
    }
    add_bytes_sent(data.size());
    return Status{};
}

Result<std::size_t> TcpStream::recv_some(ByteSpan data) {
    if (!valid()) {
        return Status(StatusCode::kClosed, "stream is not connected");
    }
    if (data.empty()) {
        return static_cast<std::size_t>(0);
    }
    const int request = static_cast<int>(data.size() > 0x40000000U ? 0x40000000U : data.size());
    const int received =
        ::recv(to_native(handle_),
               reinterpret_cast<char*>(const_cast<std::uint8_t*>(data.data())), request, 0);
    if (received == 0) {
        return static_cast<std::size_t>(0);
    }
    if (received < 0) {
        if (would_block()) {
            return Status(StatusCode::kIo, "receive timed out");
        }
        return Status(StatusCode::kIo, "receive failed");
    }
    add_bytes_received(static_cast<std::uint64_t>(received));
    return static_cast<std::size_t>(received);
}

Status TcpStream::recv_exact(ByteSpan data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const Result<std::size_t> received = recv_some(data.subspan(offset));
        if (!received.has_value()) {
            return received.error();
        }
        if (*received == 0) {
            return Status(StatusCode::kTruncated,
                          "peer closed the connection in the middle of a frame");
        }
        offset += *received;
    }
    return Status{};
}

Status TcpStream::recv_exact_interruptible(ByteSpan data, const std::atomic<bool>& stop,
                                            std::uint32_t idle_timeout_ms) {
    if (!valid()) {
        return Status(StatusCode::kClosed, "stream is not connected");
    }
    std::size_t offset = 0;
    const auto start = std::chrono::steady_clock::now();
    while (offset < data.size()) {
        if (stop.load(std::memory_order_acquire)) {
            return Status(StatusCode::kCancelled, "the server is shutting down");
        }
        if (idle_timeout_ms > 0) {
            const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (idle.count() > static_cast<std::int64_t>(idle_timeout_ms)) {
                return Status(StatusCode::kCancelled, "the connection was idle for too long");
            }
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(to_native(handle_), &readable);
        timeval wait{};
        wait.tv_sec = 0;
        wait.tv_usec = 50000;
        const int ready =
            ::select(static_cast<int>(handle_) + 1, &readable, nullptr, nullptr, &wait);
        if (ready == 0) {
            continue;
        }
        if (ready < 0) {
#ifdef _WIN32
            if (::WSAGetLastError() == WSAEINTR) {
                continue;
            }
#else
            if (errno == EINTR) {
                continue;
            }
#endif
            return Status(StatusCode::kIo, "select failed on the connection");
        }
        const std::size_t remaining = data.size() - offset;
        const int request = static_cast<int>(remaining > 0x40000000U ? 0x40000000U : remaining);
        const int received = ::recv(
            to_native(handle_),
            reinterpret_cast<char*>(const_cast<std::uint8_t*>(data.data() + offset)), request, 0);
        if (received == 0) {
            return Status(StatusCode::kTruncated,
                          "peer closed the connection in the middle of a frame");
        }
        if (received < 0) {
            return Status(StatusCode::kIo, "receive failed");
        }
        add_bytes_received(static_cast<std::uint64_t>(received));
        offset += static_cast<std::size_t>(received);
    }
    return Status{};
}

void TcpStream::discard(std::size_t count) noexcept {
    std::uint8_t scratch[512];
    while (count > 0) {
        const std::size_t chunk = count < sizeof(scratch) ? count : sizeof(scratch);
        const Result<std::size_t> received = recv_some(ByteSpan(scratch, chunk));
        if (!received.has_value() || *received == 0) {
            return;
        }
        count -= *received;
    }
}

Status TcpStream::set_read_timeout_ms(std::uint32_t milliseconds) {
    if (!valid()) {
        return Status(StatusCode::kClosed, "stream is not connected");
    }
#ifdef _WIN32
    const DWORD value = milliseconds;
    if (::setsockopt(to_native(handle_), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
        return Status(StatusCode::kIo, "cannot set the receive timeout");
    }
#else
    timeval value{};
    value.tv_sec = static_cast<time_t>(milliseconds / 1000U);
    value.tv_usec = static_cast<suseconds_t>((milliseconds % 1000U) * 1000U);
    if (::setsockopt(handle_, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) != 0) {
        return Status(StatusCode::kIo, "cannot set the receive timeout");
    }
#endif
    return Status{};
}

Status TcpStream::set_write_timeout_ms(std::uint32_t milliseconds) {
    if (!valid()) {
        return Status(StatusCode::kClosed, "stream is not connected");
    }
#ifdef _WIN32
    const DWORD value = milliseconds;
    if (::setsockopt(to_native(handle_), SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
        return Status(StatusCode::kIo, "cannot set the send timeout");
    }
#else
    timeval value{};
    value.tv_sec = static_cast<time_t>(milliseconds / 1000U);
    value.tv_usec = static_cast<suseconds_t>((milliseconds % 1000U) * 1000U);
    if (::setsockopt(handle_, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) != 0) {
        return Status(StatusCode::kIo, "cannot set the send timeout");
    }
#endif
    return Status{};
}

// ---------------------------------------------------------------------------
// TcpListener
// ---------------------------------------------------------------------------

TcpListener::~TcpListener() {
    close();
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_), port_(other.port_) {
    other.handle_ = -1;
    other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        port_ = other.port_;
        other.handle_ = -1;
        other.port_ = 0;
    }
    return *this;
}

void TcpListener::close() noexcept {
    close_native(handle_);
    handle_ = -1;
}

Result<TcpListener> TcpListener::bind_loopback(std::uint16_t port, int backlog) {
    RNF_TRYV(ensure_socket_runtime());

    const native_socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidSocket) {
        return Status(StatusCode::kIo, "cannot create a listening socket");
    }
    int reuse = 1;
    (void)::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_native(from_native(socket));
        return Status(StatusCode::kIo, "cannot bind to loopback port " + std::to_string(port));
    }
    if (::listen(socket, backlog) != 0) {
        close_native(from_native(socket));
        return Status(StatusCode::kIo, "cannot listen on the bound socket");
    }

    sockaddr_in bound{};
#ifdef _WIN32
    int length = sizeof(bound);
#else
    socklen_t length = sizeof(bound);
#endif
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        close_native(from_native(socket));
        return Status(StatusCode::kIo, "cannot read back the bound port");
    }

    TcpListener listener;
    listener.handle_ = from_native(socket);
    listener.port_ = ntohs(bound.sin_port);
    return listener;
}

Result<TcpStream> TcpListener::accept(const std::function<bool()>& should_stop) {
    if (handle_ < 0) {
        return Status(StatusCode::kClosed, "listener is closed");
    }
    for (;;) {
        if (should_stop()) {
            return Status(StatusCode::kCancelled, "listener was asked to stop");
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(to_native(handle_), &readable);
        timeval wait{};
        wait.tv_sec = 0;
        wait.tv_usec = 50000;  // 50 ms poll so shutdown stays responsive
        const int ready = ::select(static_cast<int>(handle_) + 1, &readable, nullptr, nullptr, &wait);
        if (ready == 0) {
            continue;
        }
        if (ready < 0) {
#ifdef _WIN32
            if (::WSAGetLastError() == WSAEINTR) {
                continue;
            }
#else
            if (errno == EINTR) {
                continue;
            }
#endif
            return Status(StatusCode::kIo, "select failed on the listening socket");
        }
        sockaddr_in peer{};
#ifdef _WIN32
        int length = sizeof(peer);
#else
        socklen_t length = sizeof(peer);
#endif
        const native_socket connection =
            ::accept(to_native(handle_), reinterpret_cast<sockaddr*>(&peer), &length);
        if (connection == kInvalidSocket) {
            if (should_stop()) {
                return Status(StatusCode::kCancelled, "listener was asked to stop");
            }
            return Status(StatusCode::kIo, "accept failed");
        }
        int nodelay = 1;
        (void)::setsockopt(connection, IPPROTO_TCP, TCP_NODELAY,
                           reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        TcpStream stream;
        stream.adopt(from_native(connection));
        return stream;
    }
}

Result<TcpStream> connect_loopback(const Endpoint& endpoint, std::uint32_t attempts,
                                   std::uint32_t delay_ms) {
    RNF_TRYV(ensure_socket_runtime());
    Status last(StatusCode::kIo, "no connection attempt was made");
    for (std::uint32_t attempt = 0; attempt < (attempts == 0 ? 1U : attempts); ++attempt) {
        if (attempt > 0 && delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(endpoint.port);
        if (::inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1) {
            return Status(StatusCode::kInvalidArgument,
                          "endpoint host is not a numeric IPv4 address: " + endpoint.host);
        }
        const native_socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == kInvalidSocket) {
            last = Status(StatusCode::kIo, "cannot create a client socket");
            continue;
        }
        if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close_native(from_native(socket));
            last = Status(StatusCode::kIo, "cannot connect to " + endpoint.to_text());
            continue;
        }
        int nodelay = 1;
        (void)::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                           reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        TcpStream stream;
        stream.adopt(from_native(socket));
        return stream;
    }
    return last;
}


}  // namespace rnf
