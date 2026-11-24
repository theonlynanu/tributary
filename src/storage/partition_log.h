/**
 * One partition replica's core log, as a series of segments.
 *
 * This is the only storage class that brokers talk to. Governs offsets for
 * batches, which segment is active, when to start new segments, and recovery
 * on startup.
 *
 * NOT thread safe. Broker's partition holds a mutex around every call, since
 * the log and high watermark have to change together anyway.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "storage/offset_index.h"
#include "storage/segment.h"
namespace tributary {

/**
 * Settings for a PartitionLog. Defaults are generally fine, but tests can use
 * smaller segments to check behavior.
 */
struct PartitionLogOptions {
    /**
     * New segment is started when the next batch would push the active past
     * this size. Batches larger than this get their own segment.
     */
    uint64_t segment_bytes = 64 * 1024 * 1024;

    /**
     * Roughly how many bytes of log are between index entries.
     */
    uint32_t index_interval_bytes = 4096;

    /**
     * If true, every append is fsynced before it returns. Batch will be
     * guaranteed to succeed if append succeeds and power is lost.
     *
     * If lost, Flush() must be called manually.
     */
    bool fsync_every_append = true;
};

/**
 * Result from PartitionLog::Open. Largely for logging and tests.
 */
struct RecoveryInfo {
    /**
     * If newest segment was damage and had to be truncated.
     */
    bool truncated = false;

    /**
     * Why the scan of newest segment was stopped. kEndOfSegment means a clean
     * read.
     */
    ScanStop stop = ScanStop::kEndOfSegment;

    /**
     * How many bytes were removed from the end of the newest segment.
     */
    uint64_t bytes_removed = 0;
};

/**
 * Result of PartitionLog::Read.
 */
enum class ReadStatus {
    kOk,          // 'out' holds zero or more whole batches
    kOutOfRange,  // offset is before log start or after log end
    kError        // I/O error or log is inconsistent. errno may be set.
};

class PartitionLog {
   public:
    /**
     * Opens the partition stored in dir, creating the directory and a first
     * segment if none exist.
     *
     * Every segment is found by name and opened. Sealed segments must be
     * contiguous and undamaged. Newest segment is scanned with CRC checks and
     * has damaged batches truncated and synced.
     *
     * Returns nullptr and description in *error if given when sealed segment is
     * missing or damaged.
     */
    static std::unique_ptr<PartitionLog> Open(
        const std::string& dir, const PartitionLogOptions& options,
        std::string* error = nullptr);

    PartitionLog(const PartitionLog&) = delete;
    PartitionLog& operator=(const PartitionLog&) = delete;

    /**
     * Leader: appends one batch received from a producer.
     *
     * batch must hold exactly one batch that already passed CheckBatch. It is
     * stamped with next offset and leader_epoch, then appended.
     *
     * Returns batch's base offset or -1 on failure with errno set. Log is not
     * changed on failure.
     */
    int64_t AppendAsLeader(std::vector<uint8_t>* batch, int32_t leader_epoch);

    /**
     * Follower: appends batches copied from leader, unchanged.
     *
     * data holds one or more batches back to back as carried by fetch. Every
     * batch is first checked. If any fail, nothing is appended and returns
     * false with errno EINVAL.
     */
    bool AppendAsFollower(const uint8_t* data, size_t n);

    /**
     * Reads whole batches starting with the one containing provided offset.
     *
     * Stops before any batch whose records go past max_offset (high watermark
     * for consumers, log_end_offset() for followers) and before total read is
     * beyond max_bytes (first batch is always read no matter what).
     *
     * Reading at max_offset or log_end_offset returns kOk. Offset before
     * log_start_offset or after log_end_offset returns kOutOfRange.
     */
    ReadStatus Read(int64_t offset, int64_t max_offset, size_t max_bytes,
                    std::vector<uint8_t>* out) const;

    /**
     * Discards all records at or after offset, which must be a batch boundary
     * between log_start_offset and log_end_offset.
     *
     * Used when a broker becomes a follower and drops records the new leader
     * never received, or on recovery to get rid of torn batches.
     */
    bool TruncateTo(int64_t offset);

    /**
     * Fsyncs the active segment. Only needed when fsync_every_append is false.
     */
    bool Flush();

    /**
     * Offset of the first record still in the log.
     */
    int64_t log_start_offset() const;

    /**
     * Offset the next appended record will get (one past last).
     */
    int64_t log_end_offset() const { return log_end_offset_; }

    /**
     * Number of segment files. Mostly for testing.
     */
    size_t segment_count() const { return segments_.size(); }

    /**
     * What Open found in the newest segment
     */
    const RecoveryInfo& recovery() const { return recovery_; }

    const std::string& dir() const { return dir_; }

   private:
    /**
     * A segment and the index of its batches.
     */
    struct SegmentEntry {
        std::unique_ptr<Segment> segment;
        OffsetIndex index;
    };

    PartitionLog(std::string dir, const PartitionLogOptions& options);

    /**
     * The newest segment (where appends go)
     */
    SegmentEntry& active() { return segments_.rbegin()->second; }

    /**
     * Seals active segment (syncs) and starts a new one at log_end_offset.
     */
    bool Roll();

    /**
     * Appends one stamped, checked batch holding record_count records, rolls if
     * needed, writes, syncs (if enabled), updates the index and the log end
     * offset. Log is unchanged on failure.
     */
    bool AppendBatch(const uint8_t* data, size_t n, int32_t record_count);

    std::string dir_;
    PartitionLogOptions options_;
    std::map<int64_t, SegmentEntry> segments_;
    int64_t log_end_offset_ = 0;
    RecoveryInfo recovery_;
};

}  // namespace tributary
