// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/runtime/client.hpp"

#include <string>
#include <utility>

#include "rnf/core/hash.hpp"

namespace rnf {
namespace {

Result<std::vector<std::uint8_t>> read_exact_payload(TcpStream& stream, std::uint32_t max_payload,
                                                     MessageType& type, std::uint64_t& correlation) {
    std::vector<std::uint8_t> header_bytes(kFrameHeaderSize);
    RNF_TRYV(stream.recv_exact(ByteSpan(header_bytes.data(), header_bytes.size())));
    FrameHeader header;
    RNF_TRYV(decode_frame_header(ByteSpan(header_bytes.data(), header_bytes.size()), max_payload,
                                 kProtocolVersion, header));
    std::vector<std::uint8_t> payload(header.length);
    if (header.length > 0) {
        RNF_TRYV(stream.recv_exact(ByteSpan(payload.data(), payload.size())));
    }
    if (crc32c(ByteSpan(payload.data(), payload.size())) != header.crc) {
        return Status(StatusCode::kIntegrityFailure, "response frame failed its checksum");
    }
    add_frame_received();
    type = static_cast<MessageType>(header.type);
    correlation = header.correlation;
    return payload;
}

}  // namespace

Result<Client> Client::connect(const Endpoint& endpoint, const ClientOptions& options) {
    Result<TcpStream> stream =
        connect_loopback(endpoint, options.connect_attempts, options.connect_retry_ms);
    if (!stream.has_value()) {
        return stream.error();
    }
    Client client;
    client.stream_ = std::move(*stream);
    client.options_ = options;
    RNF_TRYV(client.stream_.set_read_timeout_ms(options.io_timeout_ms));
    RNF_TRYV(client.stream_.set_write_timeout_ms(options.io_timeout_ms));
    return client;
}

Status Client::send_raw(MessageType type, ByteSpan payload) {
    FrameHeader header;
    header.version = kProtocolVersion;
    header.type = static_cast<std::uint16_t>(type);
    header.correlation = next_correlation_++;
    last_correlation_ = header.correlation;
    std::vector<std::uint8_t> frame;
    RNF_TRYV(encode_frame(header, payload, frame, options_.max_frame_payload));
    RNF_TRYV(stream_.send_all(ByteSpan(frame.data(), frame.size())));
    add_frame_sent();
    return Status{};
}

Result<std::vector<std::uint8_t>> Client::receive_raw(MessageType expected) {
    MessageType type = MessageType::kError;
    std::uint64_t correlation = 0;
    RNF_TRY(payload, read_exact_payload(stream_, options_.max_frame_payload, type, correlation));
    if (correlation != last_correlation_) {
        return Status(StatusCode::kMalformedEncoding,
                      "response correlation does not match the request");
    }
    if (type == MessageType::kError) {
        ByteReader reader(ByteSpan(payload.data(), payload.size()));
        RNF_TRY(ack, AckResponse::decode(reader));
        return Status(ack.code, ack.detail);
    }
    if (type != expected) {
        return Status(StatusCode::kUnsupported,
                      std::string("expected ") + std::string(to_string(expected)) +
                          " but received " + std::string(to_string(type)));
    }
    return payload;
}

Result<HelloResponse> Client::hello(std::uint32_t client_id) {
    HelloRequest request;
    request.protocol_version = kProtocolVersion;
    request.client_id = client_id;
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(request.encode(writer));
    RNF_TRYV(send_raw(MessageType::kHello, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kHelloAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return HelloResponse::decode(reader);
}

Result<EvidenceAckResponse> Client::submit_evidence(const std::vector<EvidenceRecord>& records) {
    SubmitEvidenceRequest request;
    request.records = records;
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(request.encode(writer));
    RNF_TRYV(send_raw(MessageType::kSubmitEvidence, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kEvidenceAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return EvidenceAckResponse::decode(reader);
}

Result<ComposeResponse> Client::compose(const TopologyGeneration& generation) {
    ComposeCommand request;
    request.generation = generation;
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(request.encode(writer));
    RNF_TRYV(send_raw(MessageType::kCompose, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kComposeAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return ComposeResponse::decode(reader);
}

Result<GrantResponse> Client::acquire(const AcquireRequest& request) {
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(request.encode(writer));
    RNF_TRYV(send_raw(MessageType::kAcquire, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kAcquireAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return GrantResponse::decode(reader);
}

Result<GrantResponse> Client::renew(const LeaseToken& token, std::uint64_t ttl_ms) {
    RenewRequest request;
    request.token = token;
    request.ttl_ms = ttl_ms;
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(request.encode(writer));
    RNF_TRYV(send_raw(MessageType::kRenew, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kRenewAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return GrantResponse::decode(reader);
}

Result<AckResponse> Client::release(const LeaseToken& token) {
    TokenMessage request;
    request.token = token;
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(request.encode(writer));
    RNF_TRYV(send_raw(MessageType::kRelease, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kReleaseAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return AckResponse::decode(reader);
}

Result<GrantResponse> Client::validate(const LeaseToken& token) {
    TokenMessage request;
    request.token = token;
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(request.encode(writer));
    RNF_TRYV(send_raw(MessageType::kValidate, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kValidateAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return GrantResponse::decode(reader);
}

Result<LookupResponse> Client::lookup(const RequestId& request) {
    LookupRequest message;
    message.request = request;
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(message.encode(writer));
    RNF_TRYV(send_raw(MessageType::kLookup, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kLookupAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return LookupResponse::decode(reader);
}

Result<StateResponse> Client::query_state() {
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(send_raw(MessageType::kQueryState, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kStateAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return StateResponse::decode(reader);
}

Result<ResourcesResponse> Client::query_resources(const ResourceQuery& query) {
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(query.encode(writer));
    RNF_TRYV(send_raw(MessageType::kQueryResources, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kResourcesAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return ResourcesResponse::decode(reader);
}

Result<PathsResponse> Client::query_paths() {
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(send_raw(MessageType::kQueryPaths, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kPathsAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return PathsResponse::decode(reader);
}

Result<GrantsResponse> Client::query_grants() {
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(send_raw(MessageType::kQueryGrants, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kGrantsAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return GrantsResponse::decode(reader);
}

Result<LifecycleResponse> Client::set_lifecycle(LifecycleState target) {
    LifecycleRequest request;
    request.target = target;
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(request.encode(writer));
    RNF_TRYV(send_raw(MessageType::kLifecycle, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kLifecycleAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return LifecycleResponse::decode(reader);
}

Result<AckResponse> Client::checkpoint() {
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(send_raw(MessageType::kCheckpoint, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kCheckpointAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return AckResponse::decode(reader);
}

Result<StatsResponse> Client::stats() {
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(send_raw(MessageType::kStats, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kStatsAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return StatsResponse::decode(reader);
}

Result<AckResponse> Client::shutdown() {
    ByteWriter writer(options_.max_frame_payload);
    RNF_TRYV(send_raw(MessageType::kShutdown, writer.span()));
    RNF_TRY(payload, receive_raw(MessageType::kShutdownAck));
    ByteReader reader(ByteSpan(payload.data(), payload.size()));
    return AckResponse::decode(reader);
}

}  // namespace rnf
