// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/persist/store.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include "rnf/core/checked.hpp"
#include "rnf/core/hash.hpp"
#include "rnf/core/log.hpp"
#include "rnf/version.hpp"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace rnf {
namespace {

constexpr char kMetaMagic[8] = {'R', 'N', 'F', 'S', 'T', 'O', 'R', '1'};
constexpr char kCheckpointMagic[8] = {'R', 'N', 'F', 'C', 'H', 'K', '0', '1'};
constexpr std::size_t kMetaSize = 256;
// Header layout: magic(8) version(4) body crc(4) body length(8) snapshot
// digest(32) ledger digest(32) wal offset(8) generation(8) epoch(8)
// incarnation(8) header crc(4) lifecycle(1) reserved(3) member set digest(32)
// reserved(32).
constexpr std::size_t kCheckpointHeaderSize = 192;
constexpr std::size_t kWalHeaderSize = 8;
constexpr std::size_t kMaxNameText = 128;
constexpr std::size_t kMaxReasonText = 128;
constexpr std::size_t kMaxDetailText = 256;

constexpr std::string_view kMetaName = "rnf.meta";
constexpr std::string_view kCheckpointName = "rnf.checkpoint";
constexpr std::string_view kCheckpointPreviousName = "rnf.checkpoint.prev";
constexpr std::string_view kWalName = "rnf.wal";
constexpr std::string_view kLockName = "rnf.lock";

/// Fixed size manifest.
struct Meta {
    std::uint32_t format_version = kStateFormatVersion;
    RackId rack{};
    std::uint64_t incarnation_counter = 0;
    RackEpoch epoch{};
    TopologyGeneration generation{};
    std::uint64_t checkpoint_offset = 0;
    std::uint64_t checkpoint_length = 0;
    Digest snapshot_digest{};
    Digest ledger_digest{};
    bool clean_shutdown = false;
    TimestampMs clean_shutdown_at_ms = 0;
    bool valid = false;
};

void store_u32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU);
    }
}

void store_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU);
    }
}

std::uint32_t load_u32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    }
    return v;
}

std::uint64_t load_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

void encode_meta(const Meta& meta, std::array<std::uint8_t, kMetaSize>& buffer) {
    buffer.fill(0);
    std::memcpy(buffer.data(), kMetaMagic, sizeof(kMetaMagic));
    store_u32(buffer.data() + 8, meta.format_version);
    store_u64(buffer.data() + 16, meta.rack.value());
    store_u64(buffer.data() + 24, meta.incarnation_counter);
    store_u64(buffer.data() + 32, meta.epoch.value);
    store_u64(buffer.data() + 40, meta.generation.value);
    store_u64(buffer.data() + 48, meta.checkpoint_offset);
    store_u64(buffer.data() + 56, meta.checkpoint_length);
    std::memcpy(buffer.data() + 64, meta.snapshot_digest.bytes.data(), 32);
    std::memcpy(buffer.data() + 96, meta.ledger_digest.bytes.data(), 32);
    buffer[128] = meta.clean_shutdown ? 1U : 0U;
    store_u64(buffer.data() + 136, meta.clean_shutdown_at_ms);
    const std::uint32_t crc =
        crc32c(ByteSpan(buffer.data(), buffer.size()));
    store_u32(buffer.data() + 12, crc);
}

Status decode_meta(const std::array<std::uint8_t, kMetaSize>& buffer, bool allow_unknown_format,
                   Meta& meta) {
    if (std::memcmp(buffer.data(), kMetaMagic, sizeof(kMetaMagic)) != 0) {
        return Status(StatusCode::kCorrupt, "store manifest has a wrong magic value");
    }
    const std::uint32_t stored_crc = load_u32(buffer.data() + 12);
    std::array<std::uint8_t, kMetaSize> copy = buffer;
    store_u32(copy.data() + 12, 0);
    if (crc32c(ByteSpan(copy.data(), copy.size())) != stored_crc) {
        return Status(StatusCode::kIntegrityFailure, "store manifest failed its checksum");
    }
    meta.format_version = load_u32(buffer.data() + 8);
    if (meta.format_version != kStateFormatVersion && !allow_unknown_format) {
        return Status(StatusCode::kIncompatibleVersion,
                      "store manifest was written by state format version " +
                          std::to_string(meta.format_version) + ", this build speaks " +
                          std::to_string(kStateFormatVersion));
    }
    meta.rack = RackId(load_u64(buffer.data() + 16));
    meta.incarnation_counter = load_u64(buffer.data() + 24);
    meta.epoch = RackEpoch{load_u64(buffer.data() + 32)};
    meta.generation = TopologyGeneration{load_u64(buffer.data() + 40)};
    meta.checkpoint_offset = load_u64(buffer.data() + 48);
    meta.checkpoint_length = load_u64(buffer.data() + 56);
    std::memcpy(meta.snapshot_digest.bytes.data(), buffer.data() + 64, 32);
    std::memcpy(meta.ledger_digest.bytes.data(), buffer.data() + 96, 32);
    meta.clean_shutdown = buffer[128] != 0U;
    meta.clean_shutdown_at_ms = load_u64(buffer.data() + 136);
    meta.valid = true;
    return Status{};
}

// -- small file helpers -----------------------------------------------------

[[nodiscard]] Status sync_file(std::FILE* file) {
    if (std::fflush(file) != 0) {
        return Status(StatusCode::kIo, "flush failed");
    }
#ifdef _WIN32
    if (::_commit(::_fileno(file)) != 0) {
        return Status(StatusCode::kIo, "commit failed");
    }
#else
    if (::fsync(::fileno(file)) != 0) {
        return Status(StatusCode::kIo, "fsync failed");
    }
#endif
    return Status{};
}

[[nodiscard]] Status write_file_atomically(const std::filesystem::path& target,
                                           const void* data, std::size_t size) {
    std::filesystem::path temporary = target;
    temporary += ".tmp";
    std::FILE* file = std::fopen(temporary.string().c_str(), "wb");
    if (file == nullptr) {
        return Status(StatusCode::kIo, "cannot create " + temporary.string());
    }
    if (size > 0 && std::fwrite(data, 1, size, file) != size) {
        std::fclose(file);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Status(StatusCode::kIo, "short write to " + temporary.string());
    }
    const Status synced = sync_file(file);
    std::fclose(file);
    if (!synced.ok()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return synced;
    }
    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return Status(StatusCode::kIo, "cannot replace " + target.string() + ": " + ec.message());
    }
    return Status{};
}

[[nodiscard]] Result<std::vector<std::uint8_t>> read_whole_file(const std::filesystem::path& path,
                                                               std::size_t max_bytes) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return Status(StatusCode::kNotFound, "cannot stat " + path.string() + ": " + ec.message());
    }
    if (size > max_bytes) {
        return Status(StatusCode::kOversizeField,
                      path.string() + " is larger than the configured bound");
    }
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return Status(StatusCode::kIo, "cannot open " + path.string());
    }
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(size));
    if (size > 0 && std::fread(buffer.data(), 1, buffer.size(), file) != buffer.size()) {
        std::fclose(file);
        return Status(StatusCode::kTruncated, "short read from " + path.string());
    }
    std::fclose(file);
    return buffer;
}

[[nodiscard]] Result<FileLock> acquire_lock(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE handle = ::CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Status(StatusCode::kBusy,
                      "another process holds the store lock at " + path.string());
    }
    return FileLock(static_cast<void*>(handle));
#else
    const int fd = ::open(path.string().c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        return Status(StatusCode::kIo, "cannot open the store lock file " + path.string());
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return Status(StatusCode::kBusy,
                      "another process holds the store lock at " + path.string());
    }
    return FileLock(reinterpret_cast<void*>(static_cast<std::intptr_t>(fd)));
#endif
}

// -- StoreRecord encoding ---------------------------------------------------

std::string_view to_string(RecordType type) noexcept {
    switch (type) {
        case RecordType::kRackBind: return "rack_bind";
        case RecordType::kConfig: return "config";
        case RecordType::kEvidence: return "evidence";
        case RecordType::kLifecycle: return "lifecycle";
        case RecordType::kGeneration: return "generation";
        case RecordType::kEpoch: return "epoch";
        case RecordType::kGrantCommit: return "grant_commit";
        case RecordType::kGrantState: return "grant_state";
        case RecordType::kRequestOutcome: return "request_outcome";
        case RecordType::kCleanShutdown: return "clean_shutdown";
    }
    return "unknown";
}

}  // namespace

Status encode(const StoreRecord& record, ByteWriter& writer) {
    writer.u8(static_cast<std::uint8_t>(record.type));
    switch (record.type) {
        case RecordType::kRackBind: {
            const auto& body = std::get<RackBindRecord>(record.payload);
            writer.u64(body.rack.value());
            writer.text(body.name, kMaxNameText);
            break;
        }
        case RecordType::kConfig: {
            const auto& body = std::get<ConfigRecord>(record.payload);
            writer.u64(body.headroom_floor.units);
            writer.u64(body.max_paths_per_pair);
            writer.u64(body.max_path_hops);
            break;
        }
        case RecordType::kEvidence: {
            RNF_TRYV(encode(std::get<EvidenceRecord>(record.payload), writer));
            break;
        }
        case RecordType::kLifecycle: {
            const auto& body = std::get<LifecycleRecord>(record.payload);
            writer.u8(static_cast<std::uint8_t>(body.state));
            writer.u64(body.generation.value);
            writer.u64(body.epoch.value);
            break;
        }
        case RecordType::kGeneration: {
            writer.u64(std::get<GenerationRecord>(record.payload).generation.value);
            break;
        }
        case RecordType::kEpoch: {
            const auto& body = std::get<EpochRecord>(record.payload);
            writer.u64(body.epoch.value);
            writer.digest(body.member_set_digest);
            break;
        }
        case RecordType::kGrantCommit: {
            RNF_TRYV(encode(std::get<Grant>(record.payload), writer));
            break;
        }
        case RecordType::kGrantState: {
            const auto& body = std::get<GrantStateRecord>(record.payload);
            writer.u64(body.id.value());
            writer.u8(static_cast<std::uint8_t>(body.state));
            writer.text(body.reason, kMaxReasonText);
            break;
        }
        case RecordType::kRequestOutcome: {
            const auto& body = std::get<RequestOutcomeRecord>(record.payload);
            writer.u64(body.request.hi);
            writer.u64(body.request.lo);
            writer.u16(static_cast<std::uint16_t>(body.code));
            writer.u64(body.grant.value());
            writer.u64(body.fence);
            writer.u64(body.incarnation.value);
            writer.digest(body.request_digest);
            writer.text(body.detail, kMaxDetailText);
            break;
        }
        case RecordType::kCleanShutdown: {
            writer.u64(std::get<ShutdownRecord>(record.payload).at_ms);
            break;
        }
    }
    if (!writer.ok()) {
        return Status(StatusCode::kOversizeField, "store record exceeds its encoding bound");
    }
    return Status{};
}

Result<StoreRecord> decode_record(ByteReader& reader) {
    StoreRecord record;
    std::uint8_t type = 0;
    if (!reader.u8(type)) {
        return Status(StatusCode::kTruncated, "store record header is truncated");
    }
    record.type = static_cast<RecordType>(type);
    switch (record.type) {
        case RecordType::kRackBind: {
            RackBindRecord body;
            std::uint64_t rack = 0;
            if (!reader.u64(rack) || !reader.text(kMaxNameText, body.name)) {
                return Status(StatusCode::kTruncated, "rack bind record is truncated");
            }
            body.rack = RackId(rack);
            record.payload = std::move(body);
            break;
        }
        case RecordType::kConfig: {
            ConfigRecord body;
            if (!reader.u64(body.headroom_floor.units) || !reader.u64(body.max_paths_per_pair) ||
                !reader.u64(body.max_path_hops)) {
                return Status(StatusCode::kTruncated, "config record is truncated");
            }
            record.payload = body;
            break;
        }
        case RecordType::kEvidence: {
            RNF_TRY(evidence, decode_evidence(reader));
            record.payload = std::move(evidence);
            break;
        }
        case RecordType::kLifecycle: {
            LifecycleRecord body;
            std::uint8_t state = 0;
            if (!reader.u8(state) || !reader.u64(body.generation.value) ||
                !reader.u64(body.epoch.value)) {
                return Status(StatusCode::kTruncated, "lifecycle record is truncated");
            }
            body.state = static_cast<LifecycleState>(state);
            record.payload = body;
            break;
        }
        case RecordType::kGeneration: {
            GenerationRecord body;
            if (!reader.u64(body.generation.value)) {
                return Status(StatusCode::kTruncated, "generation record is truncated");
            }
            record.payload = body;
            break;
        }
        case RecordType::kEpoch: {
            EpochRecord body;
            if (!reader.u64(body.epoch.value) || !reader.digest(body.member_set_digest)) {
                return Status(StatusCode::kTruncated, "epoch record is truncated");
            }
            record.payload = body;
            break;
        }
        case RecordType::kGrantCommit: {
            RNF_TRY(grant, decode_grant(reader));
            record.payload = std::move(grant);
            break;
        }
        case RecordType::kGrantState: {
            GrantStateRecord body;
            std::uint64_t id = 0;
            std::uint8_t state = 0;
            if (!reader.u64(id) || !reader.u8(state) ||
                !reader.text(kMaxReasonText, body.reason)) {
                return Status(StatusCode::kTruncated, "grant state record is truncated");
            }
            body.id = GrantId(id);
            body.state = static_cast<GrantState>(state);
            record.payload = std::move(body);
            break;
        }
        case RecordType::kRequestOutcome: {
            RequestOutcomeRecord body;
            std::uint16_t code = 0;
            std::uint64_t grant = 0;
            std::uint64_t incarnation = 0;
            if (!reader.u64(body.request.hi) || !reader.u64(body.request.lo) || !reader.u16(code) ||
                !reader.u64(grant) || !reader.u64(body.fence) || !reader.u64(incarnation) ||
                !reader.digest(body.request_digest) ||
                !reader.text(kMaxDetailText, body.detail)) {
                return Status(StatusCode::kTruncated, "request outcome record is truncated");
            }
            body.code = static_cast<StatusCode>(code);
            body.grant = GrantId(grant);
            body.incarnation = ControllerIncarnation{incarnation};
            record.payload = std::move(body);
            break;
        }
        case RecordType::kCleanShutdown: {
            ShutdownRecord body;
            if (!reader.u64(body.at_ms)) {
                return Status(StatusCode::kTruncated, "clean shutdown record is truncated");
            }
            record.payload = body;
            break;
        }
        default:
            return Status(StatusCode::kMalformedEncoding,
                          "unknown store record type " + std::to_string(type));
    }
    return record;
}

Digest store_record_digest(const StoreRecord& record) {
    ByteWriter writer(1U << 20);
    const Status status = encode(record, writer);
    if (!status.ok()) {
        Blake2s256 fallback;
        fallback.update("rnf.record.unencodable");
        fallback.update_le64(static_cast<std::uint64_t>(status.code()));
        return fallback.final();
    }
    return Blake2s256::hash(writer.span());
}

namespace {

[[nodiscard]] Result<std::vector<std::uint8_t>> build_checkpoint_bytes(const Checkpoint& checkpoint,
                                                                      std::size_t limit) {
    ByteWriter payload(limit);
    payload.u32(static_cast<std::uint32_t>(checkpoint.ledger.size()));
    for (const EvidenceRecord& record : checkpoint.ledger) {
        ByteWriter entry(512);
        RNF_TRYV(encode(record, entry));
        payload.blob(entry.span(), 512);
    }
    payload.blob(ByteSpan(checkpoint.snapshot_bytes.data(), checkpoint.snapshot_bytes.size()),
                 limit);
    if (!payload.ok()) {
        return Status(StatusCode::kResourceExhausted, "checkpoint exceeds its configured bound");
    }
    const std::vector<std::uint8_t>& body = payload.data();

    std::vector<std::uint8_t> out(kCheckpointHeaderSize + body.size());
    std::memcpy(out.data(), kCheckpointMagic, sizeof(kCheckpointMagic));
    store_u32(out.data() + 8, kStateFormatVersion);
    store_u32(out.data() + 12, crc32c(ByteSpan(body.data(), body.size())));
    store_u64(out.data() + 16, static_cast<std::uint64_t>(body.size()));
    std::memcpy(out.data() + 24, checkpoint.snapshot_digest.bytes.data(), 32);
    std::memcpy(out.data() + 56, checkpoint.ledger_digest.bytes.data(), 32);
    store_u64(out.data() + 88, checkpoint.wal_offset);
    store_u64(out.data() + 96, checkpoint.generation.value);
    store_u64(out.data() + 104, checkpoint.epoch.value);
    store_u64(out.data() + 112, checkpoint.incarnation.value);
    out[124] = static_cast<std::uint8_t>(checkpoint.lifecycle);
    std::memcpy(out.data() + 128, checkpoint.member_set_digest.bytes.data(), 32);
    store_u32(out.data() + 120, 0);
    const std::uint32_t header_crc = crc32c(ByteSpan(out.data(), kCheckpointHeaderSize));
    store_u32(out.data() + 120, header_crc);
    std::memcpy(out.data() + kCheckpointHeaderSize, body.data(), body.size());
    return out;
}

[[nodiscard]] Result<Checkpoint> parse_checkpoint(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kCheckpointHeaderSize) {
        return Status(StatusCode::kTruncated, "checkpoint is shorter than its header");
    }
    if (std::memcmp(bytes.data(), kCheckpointMagic, sizeof(kCheckpointMagic)) != 0) {
        return Status(StatusCode::kCorrupt, "checkpoint has a wrong magic value");
    }
    const std::uint32_t version = load_u32(bytes.data() + 8);
    if (version != kStateFormatVersion) {
        return Status(StatusCode::kIncompatibleVersion,
                      "checkpoint was written by state format version " +
                          std::to_string(version));
    }
    std::vector<std::uint8_t> header(bytes.begin(),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(kCheckpointHeaderSize));
    const std::uint32_t expected_header_crc = load_u32(header.data() + 120);
    store_u32(header.data() + 120, 0);
    if (crc32c(ByteSpan(header.data(), header.size())) != expected_header_crc) {
        return Status(StatusCode::kIntegrityFailure, "checkpoint header failed its checksum");
    }
    const std::uint64_t body_length = load_u64(bytes.data() + 16);
    if (body_length != bytes.size() - kCheckpointHeaderSize) {
        return Status(StatusCode::kTruncated, "checkpoint body length does not match its header");
    }
    const std::uint32_t expected_body_crc = load_u32(bytes.data() + 12);
    const std::uint8_t* body = bytes.data() + kCheckpointHeaderSize;
    if (crc32c(ByteSpan(body, static_cast<std::size_t>(body_length))) != expected_body_crc) {
        return Status(StatusCode::kIntegrityFailure, "checkpoint body failed its checksum");
    }

    Checkpoint checkpoint;
    std::memcpy(checkpoint.snapshot_digest.bytes.data(), bytes.data() + 24, 32);
    std::memcpy(checkpoint.ledger_digest.bytes.data(), bytes.data() + 56, 32);
    checkpoint.wal_offset = load_u64(bytes.data() + 88);
    checkpoint.generation = TopologyGeneration{load_u64(bytes.data() + 96)};
    checkpoint.epoch = RackEpoch{load_u64(bytes.data() + 104)};
    checkpoint.incarnation = ControllerIncarnation{load_u64(bytes.data() + 112)};
    checkpoint.lifecycle = static_cast<LifecycleState>(bytes[124]);
    std::memcpy(checkpoint.member_set_digest.bytes.data(), bytes.data() + 128, 32);

    ByteReader reader(ByteSpan(body, static_cast<std::size_t>(body_length)));
    std::uint32_t count = 0;
    if (!reader.count(count, static_cast<std::uint32_t>(default_evidence_limits().max_slots))) {
        return Status(StatusCode::kMalformedEncoding, "checkpoint ledger count exceeds its bound");
    }
    checkpoint.ledger.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        ByteSpan entry;
        if (!reader.blob(512, entry)) {
            return Status(StatusCode::kTruncated, "checkpoint ledger entry is truncated");
        }
        ByteReader entry_reader(entry);
        RNF_TRY(record, decode_evidence(entry_reader));
        checkpoint.ledger.push_back(std::move(record));
    }
    ByteSpan snapshot;
    if (!reader.blob(bytes.size(), snapshot)) {
        return Status(StatusCode::kTruncated, "checkpoint snapshot payload is truncated");
    }
    checkpoint.snapshot_bytes.assign(snapshot.begin(), snapshot.end());
    if (!reader.at_end()) {
        return Status(StatusCode::kMalformedEncoding, "trailing bytes in checkpoint payload");
    }
    checkpoint.present = true;
    return checkpoint;
}

}  // namespace

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

Store::~Store() {
    if (wal_ != nullptr) {
        std::fclose(static_cast<std::FILE*>(wal_));
        wal_ = nullptr;
    }
}

Result<std::unique_ptr<Store>> Store::open(const std::filesystem::path& dir,
                                           const StoreOptions& options) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return Status(StatusCode::kIo, "cannot create store directory " + dir.string() + ": " +
                                           ec.message());
    }

    Result<FileLock> lock = acquire_lock(dir / std::string(kLockName));
    if (!lock.has_value()) {
        return lock.error();
    }

    std::unique_ptr<Store> store(new Store());
    store->directory_ = dir;
    store->options_ = options;
    store->lock_ = std::move(*lock);

    RecoveryReport& report = store->recovered_.report;

    // -- manifest -----------------------------------------------------------
    Meta meta;
    const std::filesystem::path meta_path = dir / std::string(kMetaName);
    const std::filesystem::path wal_path = dir / std::string(kWalName);
    const bool wal_exists = std::filesystem::exists(wal_path, ec);
    report.fresh_store = !wal_exists;

    if (std::filesystem::exists(meta_path, ec)) {
        const Result<std::vector<std::uint8_t>> bytes = read_whole_file(meta_path, kMetaSize);
        Status failure;
        if (!bytes.has_value()) {
            failure = bytes.error();
        } else if (bytes->size() != kMetaSize) {
            failure = Status(StatusCode::kTruncated, "store manifest has an unexpected size");
        } else {
            std::array<std::uint8_t, kMetaSize> buffer{};
            std::memcpy(buffer.data(), bytes->data(), kMetaSize);
            failure = decode_meta(buffer, options.allow_unknown_format, meta);
        }
        if (failure.code() == StatusCode::kIncompatibleVersion) {
            // Never reinterpret a store written by a different format version.
            return failure;
        }
        if (!failure.ok()) {
            report.manifest_rejected = true;
            report.detail = failure.describe();
            report.first_failure = failure.code();
            if (!options.allow_manifest_loss) {
                // The manifest is the only durable record of the incarnation
                // counter, so a damaged one is fatal unless the operator has
                // explicitly opted into rebuilding it from the log.
                return Status(failure.code(),
                              "the store manifest at " + meta_path.string() +
                                  " is unreadable (" + failure.detail() +
                                  "); refusing to start, because the process incarnation "
                                  "counter cannot be recovered safely");
            }
            meta = Meta{};
        }
    } else if (wal_exists) {
        // A log without a manifest: the counter is rebuilt from the log itself,
        // which is conservative but cannot invent an incarnation that was never
        // written down.
        report.manifest_missing = true;
    }

    report.clean_shutdown = meta.valid && meta.clean_shutdown;

    // -- checkpoint ---------------------------------------------------------
    if (meta.valid && meta.checkpoint_length > 0 &&
        meta.checkpoint_length <= options.max_checkpoint_bytes) {
        const Result<std::vector<std::uint8_t>> bytes =
            read_whole_file(dir / std::string(kCheckpointName),
                            options.max_checkpoint_bytes + kCheckpointHeaderSize);
        if (!bytes.has_value()) {
            report.checkpoint_rejected = true;
            report.detail = bytes.error().describe();
        } else {
            const Result<Checkpoint> parsed = parse_checkpoint(*bytes);
            if (parsed.has_value()) {
                store->recovered_.checkpoint = *parsed;
                report.checkpoint_used = true;
            } else {
                report.checkpoint_rejected = true;
                report.detail = parsed.error().describe();
                report.first_failure = parsed.error().code();
            }
        }
    }

    // -- write-ahead log ----------------------------------------------------
    std::uint64_t offset = 0;
    if (report.checkpoint_used) {
        offset = store->recovered_.checkpoint.wal_offset;
    }
    if (wal_exists) {
        store->wal_ = std::fopen(wal_path.string().c_str(), "r+b");
    } else {
        store->wal_ = std::fopen(wal_path.string().c_str(), "w+b");
    }
    if (store->wal_ == nullptr) {
        return Status(StatusCode::kIo, "cannot open the write-ahead log " + wal_path.string());
    }
    std::FILE* wal = static_cast<std::FILE*>(store->wal_);
    std::error_code size_ec;
    const std::uintmax_t wal_size = std::filesystem::file_size(wal_path, size_ec);
    if (size_ec) {
        return Status(StatusCode::kIo, "cannot size the write-ahead log");
    }
    if (offset > wal_size) {
        report.checkpoint_rejected = true;
        report.detail = "checkpoint points past the end of the log";
        store->recovered_.checkpoint = Checkpoint{};
        offset = 0;
        report.checkpoint_used = false;
    }
    if (std::fseek(wal, static_cast<long>(offset), SEEK_SET) != 0) {
        return Status(StatusCode::kIo, "cannot seek in the write-ahead log");
    }

    std::uint64_t cursor = offset;
    std::uint64_t max_incarnation = 0;
    for (;;) {
        std::uint8_t header[kWalHeaderSize];
        const std::size_t header_read = std::fread(header, 1, kWalHeaderSize, wal);
        if (header_read == 0) {
            break;  // clean end of log
        }
        if (header_read < kWalHeaderSize) {
            report.truncated_tail = true;
            break;
        }
        const std::uint32_t length = load_u32(header);
        const std::uint32_t expected_crc = load_u32(header + 4);
        if (length > options.max_record_bytes) {
            report.corrupt_record = true;
            report.detail = "log record length exceeds the configured bound";
            report.first_failure = StatusCode::kOversizeField;
            break;
        }
        std::vector<std::uint8_t> payload(length);
        const std::size_t payload_read =
            length == 0 ? 0 : std::fread(payload.data(), 1, payload.size(), wal);
        if (payload_read != payload.size()) {
            report.truncated_tail = true;
            break;
        }
        if (crc32c(ByteSpan(payload.data(), payload.size())) != expected_crc) {
            const std::uintmax_t consumed = cursor + kWalHeaderSize + length;
            if (wal_size - consumed < kWalHeaderSize) {
                report.truncated_tail = true;
            } else {
                report.corrupt_record = true;
                report.first_failure = StatusCode::kIntegrityFailure;
                report.detail = "log record failed its checksum";
            }
            break;
        }
        ByteReader reader(ByteSpan(payload.data(), payload.size()));
        Result<StoreRecord> record = decode_record(reader);
        if (!record.has_value()) {
            report.corrupt_record = true;
            report.first_failure = record.error().code();
            report.detail = record.error().describe();
            break;
        }
        if (!reader.at_end()) {
            report.corrupt_record = true;
            report.first_failure = StatusCode::kMalformedEncoding;
            report.detail = "log record has trailing bytes";
            break;
        }
        if (record->type == RecordType::kGrantCommit) {
            max_incarnation = std::max(
                max_incarnation, std::get<Grant>(record->payload).incarnation.value);
        }
        store->recovered_.records.push_back(std::move(*record));
        ++report.records_replayed;
        cursor += kWalHeaderSize + length;
    }

    const std::uintmax_t discarded = wal_size > cursor ? wal_size - cursor : 0;
    report.bytes_discarded = discarded;
    if (discarded > 0) {
        // Discard the unverifiable tail so that later appends never follow a
        // partially written record.
#ifdef _WIN32
        if (::_chsize_s(::_fileno(wal), static_cast<__int64>(cursor)) != 0) {
            return Status(StatusCode::kIo, "cannot truncate the write-ahead log");
        }
#else
        if (::ftruncate(::fileno(wal), static_cast<off_t>(cursor)) != 0) {
            return Status(StatusCode::kIo, "cannot truncate the write-ahead log");
        }
#endif
        if (std::fseek(wal, static_cast<long>(cursor), SEEK_SET) != 0) {
            return Status(StatusCode::kIo, "cannot seek in the write-ahead log");
        }
    }

    store->log_bytes_ = cursor;
    store->incarnation_counter_ =
        std::max(meta.valid ? meta.incarnation_counter : 0, max_incarnation);
    store->bound_rack_ = meta.valid ? meta.rack : RackId{};
    store->checkpoint_length_ = meta.valid ? meta.checkpoint_length : 0;

    if (report.checkpoint_used) {
        store->recovered_.checkpoint.present = true;
    }
    return std::unique_ptr<Store>(std::move(store));
}

namespace {

[[nodiscard]] Status persist_meta(const std::filesystem::path& directory, const Meta& meta) {
    std::array<std::uint8_t, kMetaSize> buffer{};
    encode_meta(meta, buffer);
    return write_file_atomically(directory / std::string(kMetaName), buffer.data(), buffer.size());
}

}  // namespace

Status Store::append(const StoreRecord& record) {
    if (wal_ == nullptr) {
        return Status(StatusCode::kClosed, "the write-ahead log is not open");
    }
    ByteWriter writer(options_.max_record_bytes);
    RNF_TRYV(encode(record, writer));
    if (writer.size() > options_.max_record_bytes) {
        return Status(StatusCode::kOversizeField, "store record exceeds the configured bound");
    }

    std::uint8_t header[kWalHeaderSize];
    store_u32(header, static_cast<std::uint32_t>(writer.size()));
    store_u32(header + 4, crc32c(writer.span()));

    std::FILE* wal = static_cast<std::FILE*>(wal_);
    if (std::fwrite(header, 1, sizeof(header), wal) != sizeof(header)) {
        return Status(StatusCode::kIo, "short write of the log record header");
    }
    if (!writer.data().empty() &&
        std::fwrite(writer.data().data(), 1, writer.data().size(), wal) != writer.data().size()) {
        return Status(StatusCode::kIo, "short write of the log record body");
    }
    if (options_.fsync_on_append) {
        RNF_TRYV(sync_file(wal));
    } else if (std::fflush(wal) != 0) {
        return Status(StatusCode::kIo, "flush failed");
    }
    log_bytes_ += kWalHeaderSize + writer.size();
    ++appends_;
    return Status{};
}

Status Store::write_checkpoint(const Checkpoint& checkpoint) {
    const Result<std::vector<std::uint8_t>> bytes =
        build_checkpoint_bytes(checkpoint, options_.max_checkpoint_bytes);
    if (!bytes.has_value()) {
        return bytes.error();
    }
    const std::filesystem::path target = directory_ / std::string(kCheckpointName);
    const std::filesystem::path previous = directory_ / std::string(kCheckpointPreviousName);

    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        std::filesystem::remove(previous, ec);
        ec.clear();
        std::filesystem::rename(target, previous, ec);
        if (ec) {
            // Losing the previous checkpoint is not fatal: the manifest is only
            // updated after the new checkpoint is durable.
            RNF_LOG(LogLevel::kWarn, "could not rotate the previous checkpoint: " + ec.message());
        }
    }
    RNF_TRYV(write_file_atomically(target, bytes->data(), bytes->size()));

    Meta meta;
    meta.rack = bound_rack_;
    meta.incarnation_counter = incarnation_counter_;
    meta.epoch = checkpoint.epoch;
    meta.generation = checkpoint.generation;
    meta.checkpoint_offset = checkpoint.wal_offset;
    meta.checkpoint_length = static_cast<std::uint64_t>(bytes->size());
    meta.snapshot_digest = checkpoint.snapshot_digest;
    meta.ledger_digest = checkpoint.ledger_digest;
    meta.clean_shutdown = false;
    RNF_TRYV(persist_meta(directory_, meta));

    checkpoint_length_ = static_cast<std::uint64_t>(bytes->size());
    recovered_.checkpoint = checkpoint;
    recovered_.checkpoint.present = true;
    return Status{};
}

Status Store::mark_clean_shutdown(TimestampMs at_ms) {
    Meta meta;
    meta.rack = bound_rack_;
    meta.incarnation_counter = incarnation_counter_;
    meta.epoch = recovered_.checkpoint.epoch;
    meta.generation = recovered_.checkpoint.generation;
    meta.checkpoint_offset = recovered_.checkpoint.wal_offset;
    meta.checkpoint_length = checkpoint_length_;
    meta.snapshot_digest = recovered_.checkpoint.snapshot_digest;
    meta.ledger_digest = recovered_.checkpoint.ledger_digest;
    meta.clean_shutdown = true;
    meta.clean_shutdown_at_ms = at_ms;
    if (wal_ != nullptr) {
        RNF_TRYV(sync_file(static_cast<std::FILE*>(wal_)));
    }
    RNF_TRYV(persist_meta(directory_, meta));
    recovered_.report.clean_shutdown = true;
    return Status{};
}

Result<ControllerIncarnation> Store::next_incarnation() {
    if (incarnation_counter_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status(StatusCode::kResourceExhausted, "the incarnation counter is exhausted");
    }
    ++incarnation_counter_;
    Meta meta;
    meta.rack = bound_rack_;
    meta.incarnation_counter = incarnation_counter_;
    meta.epoch = recovered_.checkpoint.epoch;
    meta.generation = recovered_.checkpoint.generation;
    meta.checkpoint_offset = recovered_.checkpoint.wal_offset;
    meta.checkpoint_length = recovered_.checkpoint.present ? checkpoint_length_ : 0;
    meta.snapshot_digest = recovered_.checkpoint.snapshot_digest;
    meta.ledger_digest = recovered_.checkpoint.ledger_digest;
    meta.clean_shutdown = false;
    RNF_TRYV(persist_meta(directory_, meta));
    return ControllerIncarnation{incarnation_counter_};
}

Status Store::bind_rack(RackId rack) {
    bound_rack_ = rack;
    Meta meta;
    meta.rack = rack;
    meta.incarnation_counter = incarnation_counter_;
    meta.epoch = recovered_.checkpoint.epoch;
    meta.generation = recovered_.checkpoint.generation;
    meta.checkpoint_offset = recovered_.checkpoint.wal_offset;
    meta.checkpoint_length = recovered_.checkpoint.present ? checkpoint_length_ : 0;
    meta.snapshot_digest = recovered_.checkpoint.snapshot_digest;
    meta.ledger_digest = recovered_.checkpoint.ledger_digest;
    meta.clean_shutdown = false;
    return persist_meta(directory_, meta);
}

}  // namespace rnf
