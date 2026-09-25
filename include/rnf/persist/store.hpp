// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Durable state. The write-ahead log is the source of truth; checkpoints are an
// optimisation. Recovery always produces a prefix of the accepted history and
// never a state that was not durably committed.
//
// Layout of one store directory:
//
//   rnf.lock        exclusive lock, held for the lifetime of the process
//   rnf.meta        fixed size manifest: format version, rack binding, the
//                   offset and digest of the current checkpoint
//   rnf.checkpoint  ledger + composed snapshot at the manifest's offset
//   rnf.wal         append-only records, each framed and CRC protected
//
// Every record application is idempotent, so replaying a prefix of the log
// twice is safe. That is what makes a crash between the checkpoint rename and
// the manifest update harmless.

#ifndef RNF_PERSIST_STORE_HPP
#define RNF_PERSIST_STORE_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "rnf/authority/grant.hpp"
#include "rnf/model/evidence.hpp"

namespace rnf {

enum class RecordType : std::uint8_t {
    kRackBind = 1,
    kConfig = 2,
    kEvidence = 3,
    kLifecycle = 4,
    kGeneration = 5,
    kEpoch = 6,
    kGrantCommit = 7,
    kGrantState = 8,
    kRequestOutcome = 9,
    kCleanShutdown = 10,
};

[[nodiscard]] std::string_view to_string(RecordType type) noexcept;

struct RackBindRecord {
    RackId rack{};
    std::string name;
    friend bool operator==(const RackBindRecord&, const RackBindRecord&) = default;
};

struct ConfigRecord {
    Capacity headroom_floor{};
    std::uint64_t max_paths_per_pair = 2;
    std::uint64_t max_path_hops = 6;
    friend bool operator==(const ConfigRecord&, const ConfigRecord&) = default;
};

struct LifecycleRecord {
    LifecycleState state = LifecycleState::kAssembling;
    TopologyGeneration generation{};
    RackEpoch epoch{};
    friend bool operator==(const LifecycleRecord&, const LifecycleRecord&) = default;
};

struct GenerationRecord {
    TopologyGeneration generation{};
    friend bool operator==(const GenerationRecord&, const GenerationRecord&) = default;
};

struct EpochRecord {
    RackEpoch epoch{};
    Digest member_set_digest{};
    friend bool operator==(const EpochRecord&, const EpochRecord&) = default;
};

struct GrantStateRecord {
    GrantId id{};
    GrantState state = GrantState::kPending;
    std::string reason;
    friend bool operator==(const GrantStateRecord&, const GrantStateRecord&) = default;
};

struct ShutdownRecord {
    TimestampMs at_ms = 0;
    friend bool operator==(const ShutdownRecord&, const ShutdownRecord&) = default;
};

struct RequestOutcomeRecord {
    RequestId request{};
    StatusCode code = StatusCode::kOk;
    GrantId grant{};
    FencingToken fence = 0;
    ControllerIncarnation incarnation{};
    Digest request_digest{};
    std::string detail;
    friend bool operator==(const RequestOutcomeRecord&, const RequestOutcomeRecord&) = default;
};

using StorePayload =
    std::variant<RackBindRecord, ConfigRecord, EvidenceRecord, LifecycleRecord, GenerationRecord,
                 EpochRecord, Grant, GrantStateRecord, RequestOutcomeRecord, ShutdownRecord>;

struct StoreRecord {
    RecordType type = RecordType::kRackBind;
    StorePayload payload{};

    friend bool operator==(const StoreRecord&, const StoreRecord&) = default;
};

[[nodiscard]] Status encode(const StoreRecord& record, ByteWriter& writer);
[[nodiscard]] Result<StoreRecord> decode_record(ByteReader& reader);
[[nodiscard]] Digest store_record_digest(const StoreRecord& record);

struct StoreOptions {
    std::size_t max_record_bytes = 8U << 20;
    std::size_t max_checkpoint_bytes = 64U << 20;
    /// Flush the log to stable storage after every append. Turning this off is
    /// available for benchmarks only; the durability proofs assume it is on.
    bool fsync_on_append = true;
    /// Refuse to start when the store was written by an incompatible format
    /// version instead of guessing.
    bool allow_unknown_format = false;
    /// A damaged manifest is fatal by default, because the manifest is the only
    /// durable record of the process incarnation counter. Setting this to true
    /// opts into a conservative rebuild: the log is replayed from the start and
    /// the counter is derived from the highest incarnation the log mentions.
    bool allow_manifest_loss = false;
};

/// One checkpoint: the accepted evidence set and the snapshot it composed to.
struct Checkpoint {
    std::uint64_t wal_offset = 0;
    TopologyGeneration generation{};
    RackEpoch epoch{};
    LifecycleState lifecycle = LifecycleState::kAssembling;
    /// The member set digest the epoch was derived from. Without it a recovered
    /// daemon would treat the first composition as a membership change and
    /// advance the epoch, fencing every lease for no reason.
    Digest member_set_digest{};
    ControllerIncarnation incarnation{};
    Digest snapshot_digest{};
    Digest ledger_digest{};
    std::vector<EvidenceRecord> ledger;
    std::vector<std::uint8_t> snapshot_bytes;
    bool present = false;
};

struct RecoveryReport {
    bool fresh_store = false;
    bool truncated_tail = false;      // a partial record at the end of the log
    bool corrupt_record = false;      // a record failed its CRC after valid data
    bool checkpoint_used = false;     // a valid checkpoint was loaded
    bool checkpoint_rejected = false; // a checkpoint existed but failed integrity
    bool clean_shutdown = false;
    bool manifest_rejected = false;   // the manifest existed but was not readable
    bool manifest_missing = false;    // no manifest at all; counter rebuilt from the log
    std::size_t records_replayed = 0;
    std::uint64_t bytes_discarded = 0;
    StatusCode first_failure = StatusCode::kOk;
    std::string detail;
};

struct RecoveredState {
    Checkpoint checkpoint;
    std::vector<StoreRecord> records;
    RecoveryReport report;
};

/// An exclusive lock over a store directory.
///
/// Threading: the lock is owned by the opening thread and released when the
/// object is destroyed. The operating system releases it if the process dies,
/// so a hard kill never leaves a lock that blocks the next start.
class FileLock {
public:
    FileLock() = default;
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;

    /// Adopt an already acquired platform handle. Internal: used by Store.
    explicit FileLock(void* handle) noexcept : handle_(handle) {}

    [[nodiscard]] bool held() const noexcept;
    void release() noexcept;

private:
    void* handle_ = nullptr;  // platform handle, intentionally opaque
};

/// Threading: Store is not internally synchronised. rnfd serialises all access
/// under its state mutex; appends and checkpoints never run concurrently.
class Store {
public:
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    /// Open (creating if needed) a store directory and replay it.
    [[nodiscard]] static Result<std::unique_ptr<Store>> open(const std::filesystem::path& dir,
                                                             const StoreOptions& options);

    /// Append one record. Returns only after the record is durably committed
    /// when fsync_on_append is set.
    [[nodiscard]] Status append(const StoreRecord& record);

    /// Rewrite the checkpoint. The previous checkpoint is kept until the new
    /// one is durable.
    [[nodiscard]] Status write_checkpoint(const Checkpoint& checkpoint);

    /// Mark the store cleanly closed. The next open reports clean_shutdown.
    [[nodiscard]] Status mark_clean_shutdown(TimestampMs at_ms);

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    [[nodiscard]] std::uint64_t log_bytes() const noexcept { return log_bytes_; }
    [[nodiscard]] std::size_t appends() const noexcept { return appends_; }
    [[nodiscard]] const RecoveryReport& recovery() const noexcept {
        return recovered_.report;
    }
    [[nodiscard]] const Checkpoint& checkpoint() const noexcept { return recovered_.checkpoint; }
    /// Everything the store replayed at open time. The caller applies these
    /// records to rebuild the daemon state.
    [[nodiscard]] const RecoveredState& recovered() const noexcept { return recovered_; }
    [[nodiscard]] std::uint64_t incarnation_counter() const noexcept {
        return incarnation_counter_;
    }
    /// Bump and persist the process incarnation counter. Strictly increasing
    /// across restarts of the same store, which is what fences stale authority.
    [[nodiscard]] Result<ControllerIncarnation> next_incarnation();

    /// Record which rack this store belongs to. A store opened for a different
    /// rack is refused by the caller, not silently reused.
    [[nodiscard]] Status bind_rack(RackId rack);
    [[nodiscard]] RackId bound_rack() const noexcept { return bound_rack_; }
    [[nodiscard]] std::uint64_t checkpoint_length() const noexcept { return checkpoint_length_; }

private:
    Store() = default;

    std::filesystem::path directory_;
    StoreOptions options_;
    FileLock lock_;
    void* wal_ = nullptr;  // FILE*
    std::uint64_t log_bytes_ = 0;
    std::size_t appends_ = 0;
    RecoveredState recovered_;
    std::uint64_t incarnation_counter_ = 0;
    std::uint64_t checkpoint_length_ = 0;
    RackId bound_rack_{};
};

}  // namespace rnf

#endif  // RNF_PERSIST_STORE_HPP
