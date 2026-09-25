// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/runtime/server.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include "rnf/core/hash.hpp"
#include "rnf/core/log.hpp"

namespace rnf {
namespace {

/// Send a message: encode the body, wrap it in a frame, write it out.
template <class Message>
Status write_message(TcpStream& stream, MessageType type, std::uint64_t correlation,
                     const Message& message, std::uint32_t max_payload) {
    ByteWriter writer(max_payload);
    RNF_TRYV(message.encode(writer));
    FrameHeader header;
    header.version = kProtocolVersion;
    header.type = static_cast<std::uint16_t>(type);
    header.correlation = correlation;
    std::vector<std::uint8_t> frame;
    RNF_TRYV(encode_frame(header, writer.span(), frame, max_payload));
    RNF_TRYV(stream.send_all(ByteSpan(frame.data(), frame.size())));
    add_frame_sent();
    return Status{};
}

/// Answer one request. A command that fails is reported to the caller as a
/// typed error frame; the connection stays usable. Only a framing failure ends
/// the session, because after a bad frame the stream cannot be resynchronised.
template <class Response, class Call>
Status answer(TcpStream& stream, MessageType type, std::uint64_t correlation, Call&& call,
              std::uint32_t max_payload) {
    Result<Response> response = call();
    if (!response.has_value()) {
        AckResponse failure;
        failure.code = response.error().code();
        failure.detail = response.error().detail();
        return write_message(stream, MessageType::kError, correlation, failure, max_payload);
    }
    return write_message(stream, type, correlation, *response, max_payload);
}

}  // namespace

DaemonServer::DaemonServer(DaemonCore& core, ServerConfig config)
    : core_(core), config_(std::move(config)) {}

DaemonServer::~DaemonServer() {
    request_stop();
    if (ticker_.joinable()) {
        ticker_stop_.store(true, std::memory_order_release);
        ticker_.join();
    }
    reap_workers(true);
    listener_.close();
}

Status DaemonServer::bind(const std::string& host, std::uint16_t port) {
    if (host != "127.0.0.1" && host != "localhost") {
        return Status(StatusCode::kUnsupported,
                      "this build only binds the loopback address; refusing to bind " + host);
    }
    Result<TcpListener> listener = TcpListener::bind_loopback(port, 32);
    if (!listener.has_value()) {
        return listener.error();
    }
    listener_ = std::move(*listener);
    return Status{};
}

void DaemonServer::reap_workers(bool join_all) {
    std::vector<Worker> keep;
    keep.reserve(workers_.size());
    for (Worker& worker : workers_) {
        const bool done = worker.finished->load(std::memory_order_acquire);
        if (done || join_all) {
            if (worker.thread.joinable()) {
                // Joining happens without the daemon mutex held: a worker that
                // is still inside a command must be able to finish.
                worker.thread.join();
            }
        } else {
            keep.push_back(std::move(worker));
        }
    }
    workers_ = std::move(keep);
}

Status DaemonServer::send_error(TcpStream& stream, std::uint64_t correlation, StatusCode code,
                                const std::string& detail) {
    AckResponse message;
    message.code = code;
    message.detail = detail;
    return write_message(stream, MessageType::kError, correlation, message,
                         config_.max_frame_payload);
}

Status DaemonServer::run(std::atomic<bool>* external_stop) {
    if (!listener_.valid()) {
        return Status(StatusCode::kInvalidArgument, "the server has no listening socket");
    }
    ticker_ = std::thread([this]() {
        while (!ticker_stop_.load(std::memory_order_acquire) &&
               !stop_.load(std::memory_order_acquire)) {
            const TimestampMs now = static_cast<TimestampMs>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            (void)core_.expire_due(now);
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.tick_interval_ms));
        }
    });

    const auto should_stop = [this, external_stop]() {
        if (stop_.load(std::memory_order_acquire)) {
            return true;
        }
        return external_stop != nullptr && external_stop->load(std::memory_order_acquire);
    };
    while (!should_stop()) {
        Result<TcpStream> accepted = listener_.accept(should_stop);
        if (!accepted.has_value()) {
            if (accepted.error().code() == StatusCode::kCancelled) {
                break;
            }
            return accepted.error();
        }
        reap_workers(false);
        // Reserve the slot here rather than inside the worker, so the bound is
        // exact even when several connections are accepted back to back. The
        // worker releases it on the way out.
        std::size_t observed = active_connections_.load(std::memory_order_acquire);
        bool reserved = false;
        while (observed < config_.max_connections) {
            if (active_connections_.compare_exchange_weak(observed, observed + 1,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
                reserved = true;
                break;
            }
        }
        if (!reserved) {
            core_.note_connection_rejected();
            accepted->close();
            continue;
        }
        core_.note_connection_accepted();
        auto finished = std::make_shared<std::atomic<bool>>(false);
        const std::lock_guard<std::mutex> guard(workers_mutex_);
        workers_.push_back(
            Worker{std::thread([this, stream = std::move(*accepted), finished]() mutable {
                       handle_connection(std::move(stream), finished);
                   }),
                   finished});
    }

    stop_.store(true, std::memory_order_release);
    ticker_stop_.store(true, std::memory_order_release);
    if (ticker_.joinable()) {
        ticker_.join();
    }
    listener_.close();
    reap_workers(true);
    return Status{};
}

void DaemonServer::handle_connection(TcpStream stream, std::shared_ptr<std::atomic<bool>> finished) {
    (void)stream.set_read_timeout_ms(config_.idle_timeout_ms);
    (void)stream.set_write_timeout_ms(config_.idle_timeout_ms);

    std::vector<std::uint8_t> header_bytes(kFrameHeaderSize);
    std::vector<std::uint8_t> payload;
    for (;;) {
        const Status header_status = stream.recv_exact_interruptible(
            ByteSpan(header_bytes.data(), header_bytes.size()), stop_, config_.idle_timeout_ms);
        if (!header_status.ok()) {
            break;  // orderly close, reset, cancellation or idle timeout
        }
        FrameHeader header;
        const Status decoded =
            decode_frame_header(ByteSpan(header_bytes.data(), header_bytes.size()),
                                config_.max_frame_payload, kProtocolVersion, header);
        if (!decoded.ok()) {
            add_frame_rejected();
            core_.note_frame_rejected();
            // The length field may be untrustworthy, so the only safe recovery
            // is to answer with a typed error and drop the connection.
            (void)send_error(stream, header.correlation, decoded.code(), decoded.detail());
            break;
        }
        payload.assign(header.length, 0);
        if (header.length > 0) {
            const Status body_status = stream.recv_exact_interruptible(
                ByteSpan(payload.data(), payload.size()), stop_, config_.idle_timeout_ms);
            if (!body_status.ok()) {
                add_frame_rejected();
                core_.note_frame_rejected();
                break;
            }
        }
        if (crc32c(ByteSpan(payload.data(), payload.size())) != header.crc) {
            add_frame_rejected();
            core_.note_frame_rejected();
            (void)send_error(stream, header.correlation, StatusCode::kIntegrityFailure,
                             "frame payload failed its checksum");
            break;
        }
        add_frame_received();
        const Status dispatched =
            dispatch(stream, static_cast<MessageType>(header.type), header.correlation,
                     ByteSpan(payload.data(), payload.size()));
        if (!dispatched.ok()) {
            break;
        }
        if (stop_.load(std::memory_order_acquire)) {
            break;
        }
    }
    stream.close();
    active_connections_.fetch_sub(1, std::memory_order_acq_rel);
    finished->store(true, std::memory_order_release);
}

Status DaemonServer::dispatch(TcpStream& stream, MessageType type, std::uint64_t correlation,
                              ByteSpan payload) {
    core_.note_request_served();
    const std::uint32_t limit = config_.max_frame_payload;
    ByteReader reader(payload);

    // A failure to decode the body is a framing problem: report it and then let
    // the caller close the connection, because the stream cannot be trusted to
    // be back in sync.
    const auto undecodable = [&](const Status& status) -> Status {
        (void)send_error(stream, correlation, status.code(), status.detail());
        add_frame_rejected();
        core_.note_frame_rejected();
        return status;
    };

    switch (type) {
        case MessageType::kHello: {
            Result<HelloRequest> request = HelloRequest::decode(reader);
            if (!request.has_value()) {
                return undecodable(request.error());
            }
            Result<HelloResponse> response = core_.hello(*request);
            if (!response.has_value()) {
                return undecodable(response.error());
            }
            if (!response->accepted) {
                RNF_TRYV(write_message(stream, MessageType::kHelloAck, correlation, *response,
                                       limit));
                return Status(StatusCode::kIncompatibleVersion, response->detail);
            }
            return write_message(stream, MessageType::kHelloAck, correlation, *response, limit);
        }
        case MessageType::kSubmitEvidence: {
            Result<SubmitEvidenceRequest> request = SubmitEvidenceRequest::decode(reader);
            if (!request.has_value()) {
                return undecodable(request.error());
            }
            return answer<EvidenceAckResponse>(
                stream, MessageType::kEvidenceAck, correlation,
                [&]() { return core_.submit_evidence(*request); }, limit);
        }
        case MessageType::kCompose: {
            Result<ComposeCommand> request = ComposeCommand::decode(reader);
            if (!request.has_value()) {
                return undecodable(request.error());
            }
            return answer<ComposeResponse>(
                stream, MessageType::kComposeAck, correlation,
                [&]() { return core_.compose(*request); }, limit);
        }
        case MessageType::kAcquire: {
            Result<AcquireRequest> request = AcquireRequest::decode(reader);
            if (!request.has_value()) {
                return undecodable(request.error());
            }
            return answer<GrantResponse>(
                stream, MessageType::kAcquireAck, correlation,
                [&]() { return core_.acquire(*request); }, limit);
        }
        case MessageType::kRenew: {
            Result<RenewRequest> request = RenewRequest::decode(reader);
            if (!request.has_value()) {
                return undecodable(request.error());
            }
            return answer<GrantResponse>(
                stream, MessageType::kRenewAck, correlation,
                [&]() { return core_.renew(*request); }, limit);
        }
        case MessageType::kRelease: {
            Result<TokenMessage> request = TokenMessage::decode(reader);
            if (!request.has_value()) {
                return undecodable(request.error());
            }
            return answer<AckResponse>(
                stream, MessageType::kReleaseAck, correlation,
                [&]() { return core_.release(*request); }, limit);
        }
        case MessageType::kValidate: {
            Result<TokenMessage> request = TokenMessage::decode(reader);
            if (!request.has_value()) {
                return undecodable(request.error());
            }
            return answer<GrantResponse>(
                stream, MessageType::kValidateAck, correlation,
                [&]() { return core_.validate(*request); }, limit);
        }
        case MessageType::kLookup: {
            Result<LookupRequest> request = LookupRequest::decode(reader);
            if (!request.has_value()) {
                return undecodable(request.error());
            }
            return answer<LookupResponse>(
                stream, MessageType::kLookupAck, correlation,
                [&]() { return core_.lookup(*request); }, limit);
        }
        case MessageType::kQueryState: {
            return answer<StateResponse>(stream, MessageType::kStateAck, correlation,
                                         [&]() { return core_.query_state(); }, limit);
        }
        case MessageType::kQueryResources: {
            Result<ResourceQuery> request = ResourceQuery::decode(reader);
            if (!request.has_value()) {
                return undecodable(request.error());
            }
            return answer<ResourcesResponse>(
                stream, MessageType::kResourcesAck, correlation,
                [&]() { return core_.query_resources(*request); }, limit);
        }
        case MessageType::kQueryPaths: {
            return answer<PathsResponse>(stream, MessageType::kPathsAck, correlation,
                                         [&]() { return core_.query_paths(); }, limit);
        }
        case MessageType::kQueryGrants: {
            return answer<GrantsResponse>(stream, MessageType::kGrantsAck, correlation,
                                          [&]() { return core_.query_grants(); }, limit);
        }
        case MessageType::kLifecycle: {
            Result<LifecycleRequest> request = LifecycleRequest::decode(reader);
            if (!request.has_value()) {
                return undecodable(request.error());
            }
            return answer<LifecycleResponse>(
                stream, MessageType::kLifecycleAck, correlation,
                [&]() { return core_.set_lifecycle(*request); }, limit);
        }
        case MessageType::kCheckpoint: {
            return answer<AckResponse>(stream, MessageType::kCheckpointAck, correlation,
                                       [&]() { return core_.checkpoint(); }, limit);
        }
        case MessageType::kStats: {
            return answer<StatsResponse>(stream, MessageType::kStatsAck, correlation,
                                         [&]() { return core_.stats(); }, limit);
        }
        case MessageType::kShutdown: {
            AckResponse response;
            response.code = StatusCode::kOk;
            response.detail = "the daemon is shutting down";
            RNF_TRYV(write_message(stream, MessageType::kShutdownAck, correlation, response,
                                   config_.max_frame_payload));
            request_stop();
            return Status{};
        }
        case MessageType::kHelloAck:
        case MessageType::kEvidenceAck:
        case MessageType::kComposeAck:
        case MessageType::kAcquireAck:
        case MessageType::kRenewAck:
        case MessageType::kReleaseAck:
        case MessageType::kValidateAck:
        case MessageType::kLookupAck:
        case MessageType::kStateAck:
        case MessageType::kResourcesAck:
        case MessageType::kPathsAck:
        case MessageType::kGrantsAck:
        case MessageType::kLifecycleAck:
        case MessageType::kCheckpointAck:
        case MessageType::kStatsAck:
        case MessageType::kShutdownAck:
        case MessageType::kError:
            add_frame_rejected();
            core_.note_frame_rejected();
            return send_error(stream, correlation, StatusCode::kUnsupported,
                              "a response message type arrived at the server");
    }
    return undecodable(Status(StatusCode::kUnsupported, "unhandled message type"));
}

}  // namespace rnf

