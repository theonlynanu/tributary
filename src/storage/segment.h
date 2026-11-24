/**
 * One segment of a partition's log.
 *
 * Segments are single files at <dir>/<base_offset, 20 digits>.log, and hold
 * record batches in sequence with consecutive offsets. The first bath's base
 * offset matches the number in the file name. The partition log is the full
 * series of segments. Appends only happen on the newest segment.
 *
 * Segment can append bytes, read bytes, iterate through batch headers, and
 * shorten the file. It doesn't govern where offsets go, when new segments are
 * started, or when to fsync. These are handled by the PartitionLog.
 *
 * I use factories here instead of directly opening files to force failures and
 * simplify error handling later.
 *
 * NOT thread-safe: Append and Truncate change size(), so every call should
 * happen while holding the partition's lock.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "storage/file_io.h"
#include "storage/record_batch.h"

namespace tributary {

/**
 * Returns the file name for a segment starting at base_offset.
 *
 * File names are padded to 20 digits with extension .log
 */
std::string SegmentFileName(int64_t base_offset);

/**
 * Parses a name produced by SegmentFileName.
 *
 * Accepts exactly 20 digits followed by .log (value must fit into a
 * int64_t), and stores the value in base_offset. Returns false for anything
 * else, allowing directory traversal to skip all other files.
 */
bool ParseSegmentFileName(const std::string& name, int64_t* base_offset);

/**
 * Why a Segment::Scan stopped.
 */
enum class ScanStop {
    kEndOfSegment,       // read every batch up to size()
    kStoppedByCallback,  // callback returned false
    kIncomplete,         // batch runs past EOF (torn write)
    kCorrupt,            // bad header, checksum doesn't match
    kOffsetMismatch,     // base offset is not the one expected
    kIoError             // read failed, errno should be set
};

/**
 * Result of a Segment::Scan.
 *
 * valid_end is the byte position just after the last accepted batch.
 * next_offset is the base offset the following batch should have.
 *
 * When scan stops on damage, that location is where recovery truncates the file
 * and where the log's end offset resumes.
 */
struct ScanResult {
    uint64_t valid_end;
    int64_t next_offset;
    ScanStop stop;

    /**
     * True if the scan stopped because the file was damaged at valid_end.
     *
     * Not set for I/O errors (since those don't reflect the data itself).
     */
    bool damaged() const {
        return stop == ScanStop::kIncomplete || stop == ScanStop::kCorrupt ||
               stop == ScanStop::kOffsetMismatch;
    }
};

class Segment {
   public:
    /**
     * Called by Scan for each accepted batch, with its header and the byte
     * position where it starts. Returns true to continue, false to stop.
     */
    using ScanFn = std::function<bool(const BatchHeader&, uint64_t position)>;

    /**
     * Creates a new, empty segment file in dir for base_offset.
     *
     * Fails if file already exists (no overwriting) and syncs dir so the new
     * file is written to disk.
     *
     * Returns nullptr on failure with errno set.
     */
    static std::unique_ptr<Segment> Create(const std::string& dir,
                                           int64_t base_offset);

    /**
     * Opens an existing segment file in dir for base_offset.
     *
     * Size is taken from the file as-is. Validation happens in Scan.
     * Returns nullptr if the file is missing or cannot be opened with errno set
     */
    static std::unique_ptr<Segment> OpenExisting(const std::string& dir,
                                                 int64_t base_offset);

    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    /**
     * Offset of first batch in this segment.
     */
    int64_t base_offset() const { return base_offset_; }

    /**
     * Current file size in bytes (position where next append goes).
     */
    uint64_t size() const { return size_; }

    /**
     * Full path of segment file.
     */
    const std::string& path() const { return path_; }

    /**
     * Writes n bytes at end of file and increases size.
     *
     * Does not fsync (call Sync if durability needed). On failure, any partly
     * written byets are cut off.
     */
    bool Append(const uint8_t* data, size_t n);

    /**
     * Read n bytes starting from position into out.
     *
     * Returns false if the range doesn't lie within size() or on an I/O error.
     */
    bool ReadAt(uint64_t position, size_t n, std::vector<uint8_t>* out) const;

    /**
     * Shortens file to new_size bytes, which cannot exceed size().
     *
     * Does not fsync (call Sync if durability needed). On failure, any partly
     * written byets are cut off.
     */
    bool Truncate(uint64_t new_size);

    /**
     * Flushes file contents and size to stable storage.
     */
    bool Sync();

    /**
     * Walks through batches from from_position (which must be the start of a
     * batch).
     *
     * For each batch, validates the header, checks that batch fits within
     * size(), and that base_offset = expected_offset (previous batch's
     * next_offset).
     *
     * Accepted batches are passed to fn. Scan either stops at end of segment,
     * when fn returns false, at the first bad batch, or on I/O errors.
     *
     * Uses a callback so that it can be used by recovery, read, and index
     * rebuilding, without needing to write the same loop multiple times.
     */
    ScanResult Scan(uint64_t from_position, int64_t expected_offset,
                    bool verify_crc, const ScanFn& fn) const;

   private:
    Segment(std::string path, UniqueFd fd, int64_t base_offset, uint64_t size);

    std::string path_;
    UniqueFd fd_;
    int64_t base_offset_;
    uint64_t size_;
};

}  // namespace tributary
