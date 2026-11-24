#include "partition_log.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "storage/file_io.h"
#include "storage/offset_index.h"
#include "storage/record_batch.h"
#include "storage/segment.h"

namespace tributary {
namespace {

void SetError(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
}

std::string ErrnoMessage(const std::string& what) {
    return what + ": " + std::strerror(errno);
}

const char* StopName(ScanStop s) {
    switch (s) {
        case ScanStop::kEndOfSegment:
            return "end of segment";
        case ScanStop::kStoppedByCallback:
            return "stopped by callback";
        case ScanStop::kIncomplete:
            return "incomplete batch";
        case ScanStop::kCorrupt:
            return "corrupted batch";
        case ScanStop::kOffsetMismatch:
            return "offset out of sequence";
        case ScanStop::kIoError:
            return "I/O error";
    }
    return "unknown";
}

}  // namespace

PartitionLog::PartitionLog(std::string dir, const PartitionLogOptions& options)
    : dir_(std::move(dir)), options_(options) {}

std::unique_ptr<PartitionLog> PartitionLog::Open(
    const std::string& dir, const PartitionLogOptions& options,
    std::string* error) {
    std::unique_ptr<PartitionLog> log(new PartitionLog(dir, options));

    // Check directory exists
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        SetError(error, "create " + dir + ": " + ec.message());
        return nullptr;
    }

    // Find segment files
    std::vector<int64_t> bases;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        int64_t base = 0;
        if (entry.is_regular_file() &&
            ParseSegmentFileName(entry.path().filename().string(), &base)) {
            bases.push_back(base);
        }
    }
    if (ec) {
        SetError(error, "list " + dir + ": " + ec.message());
        return nullptr;
    }

    std::sort(bases.begin(), bases.end());

    // Create new segments for new partition
    if (bases.empty()) {
        auto seg = Segment::Create(dir, 0);
        if (!seg) {
            SetError(error, ErrnoMessage("create first segment in " + dir));
            return nullptr;
        }

        log->segments_.emplace(
            0, SegmentEntry{std::move(seg),
                            OffsetIndex(0, options.index_interval_bytes)});
        log->log_end_offset_ = 0;
        return log;
    }

    // Open every segment, oldest first, and check they start where previous
    // ended
    int64_t expected = bases.front();
    for (size_t i = 0; i < bases.size(); i++) {
        const int64_t base = bases[i];
        const bool newest = (i + 1 == bases.size());

        if (base != expected) {
            SetError(error, dir + ": segment " + SegmentFileName(base) +
                                " starts at " + std::to_string(base) +
                                " but offset " + std::to_string(expected) +
                                " was expected.");
            return nullptr;
        }

        auto seg = Segment::OpenExisting(dir, base);
        if (!seg) {
            SetError(error, ErrnoMessage("open " + SegmentFileName(base)));
            return nullptr;
        }

        // Rebuild the index while scanning. Only newest segment can have been
        // mid-write on crash, so it gets full CRC check
        OffsetIndex index(base, options.index_interval_bytes);
        const ScanResult r = seg->Scan(
            0, base, newest, [&index](const BatchHeader& h, uint64_t pos) {
                index.MaybeAdd(h.base_offset, pos);
                return true;
            });

        if (r.stop == ScanStop::kIoError) {
            SetError(error, ErrnoMessage("read " + seg->path()));
            return nullptr;
        }

        if (r.damaged()) {
            if (!newest) {
                // Damage in a sealed segment means deeper error
                SetError(error, seg->path() + ": " + StopName(r.stop) +
                                    " at byte " + std::to_string(r.valid_end) +
                                    " in a sealed segment");
                return nullptr;
            }

            const uint64_t old_size = seg->size();
            if (!seg->Truncate(r.valid_end) || !seg->Sync()) {
                SetError(error, ErrnoMessage("truncate " + seg->path()));
                return nullptr;
            }

            log->recovery_.truncated = true;
            log->recovery_.stop = r.stop;
            log->recovery_.bytes_removed = old_size - r.valid_end;
        }

        expected = r.next_offset;
        log->segments_.emplace(base,
                               SegmentEntry{std::move(seg), std::move(index)});
    }

    log->log_end_offset_ = expected;
    return log;
}

int64_t PartitionLog::log_start_offset() const {
    return segments_.begin()->first;
}

bool PartitionLog::Roll() {
    // Seal current segment
    if (!active().segment->Sync()) return false;

    auto seg = Segment::Create(dir_, log_end_offset_);
    if (!seg) return false;
    segments_.emplace(log_end_offset_,
                      SegmentEntry{std::move(seg),
                                   OffsetIndex(log_end_offset_,
                                               options_.index_interval_bytes)});

    return true;
}

bool PartitionLog::AppendBatch(const uint8_t* data, size_t n,
                               int32_t record_count) {
    // Start a new segment if this batch won't fit
    {
        const Segment& seg = *active().segment;
        if (seg.size() > 0 && seg.size() + n > options_.segment_bytes) {
            if (!Roll()) return false;
        }
    }

    SegmentEntry& entry = active();
    const uint64_t position = entry.segment->size();

    if (!entry.segment->Append(data, n)) return false;
    if (options_.fsync_every_append && !entry.segment->Sync()) {
        // Bytes are in file but not synced. Remove so failed append doesn't
        // affect log
        const int saved = errno;
        entry.segment->Truncate(position);
        errno = saved;
        return false;
    }

    entry.index.MaybeAdd(log_end_offset_, position);
    log_end_offset_ += record_count;
    return true;
}

int64_t PartitionLog::AppendAsLeader(std::vector<uint8_t>* batch,
                                     int32_t leader_epoch) {
    BatchHeader h{};
    if (ReadBatchHeader(batch->data(), batch->size(), &h) != BatchStatus::kOk ||
        h.total_size() != batch->size()) {
        errno = EINVAL;
        return -1;
    }

    const int64_t base = log_end_offset_;
    StampBatch(batch->data(), base, leader_epoch);
    if (!AppendBatch(batch->data(), batch->size(), h.record_count)) return -1;
    return base;
}

bool PartitionLog::AppendAsFollower(const uint8_t* data, size_t n) {
    // Check every batch before appending any, so a bad batch in the middle
    // can't leave half of a fetch response in the log.
    struct Pending {
        size_t position;
        size_t size;
        int32_t record_count;
    };
    std::vector<Pending> pending;

    size_t pos = 0;
    int64_t expected = log_end_offset_;
    while (pos < n) {
        BatchHeader h{};
        if (CheckBatch(data + pos, n - pos, &h) != BatchStatus::kOk ||
            h.base_offset != expected) {
            errno = EINVAL;
            return false;
        }
        pending.push_back({pos, h.total_size(), h.record_count});
        pos += h.total_size();
        expected = h.next_offset();
    }

    for (const Pending& p : pending) {
        if (!AppendBatch(data + p.position, p.size, p.record_count))
            return false;
    }
    return true;
}

ReadStatus PartitionLog::Read(int64_t offset, int64_t max_offset,
                              size_t max_bytes,
                              std::vector<uint8_t>* out) const {
    out->clear();

    if (offset < log_start_offset() || offset > log_end_offset_) {
        return ReadStatus::kOutOfRange;
    }
    if (max_offset > log_end_offset_) {
        max_offset = log_end_offset_;
    }
    if (offset >= max_offset) return ReadStatus::kOk;  // Nothing to read yet

    // Find segment holding offset, then the batchholding it
    auto it = segments_.upper_bound(offset);
    if (it == segments_.begin()) return ReadStatus::kError;
    it--;

    const OffsetIndex::Entry hint = it->second.index.Lookup(offset);
    bool found = false;
    uint64_t start_position = 0;
    int64_t start_offset = 0;
    const ScanResult located =
        it->second.segment->Scan(hint.position, hint.offset, false,
                                 [&](const BatchHeader& h, uint64_t pos) {
                                     if (h.last_offset() >= offset) {
                                         found = true;
                                         start_position = pos;
                                         start_offset = h.base_offset;
                                         return false;
                                     }
                                     return true;
                                 });
    if (!found) {
        if (located.stop != ScanStop::kIoError) errno = 0;
        return ReadStatus::kError;
    }

    // Gather whole batches, continuing into later segments until one goes past
    // max.
    bool full = false;
    bool first_segment = true;

    for (; it != segments_.end() && !full; it++) {
        const Segment& seg = *it->second.segment;
        const uint64_t from = first_segment ? start_position : 0;
        const int64_t from_offset =
            first_segment ? start_offset : seg.base_offset();
        first_segment = false;
        uint64_t bytes = 0;

        const ScanResult r = seg.Scan(
            from, from_offset, false, [&](const BatchHeader& h, uint64_t) {
                if (h.next_offset() > max_offset) {
                    full = true;
                    return false;
                }

                const bool first_batch = out->empty() && bytes == 0;
                if (!first_segment &&
                    out->size() + bytes + h.total_size() > max_bytes) {
                    full = true;
                    return false;
                }
                bytes += h.total_size();
                return true;
            });

        if (r.stop == ScanStop::kIoError || r.damaged()) {
            return ReadStatus::kError;
        }

        if (bytes > 0) {
            std::vector<uint8_t> chunk;
            if (!seg.ReadAt(from, bytes, &chunk)) {
                return ReadStatus::kError;
            }
            out->insert(out->end(), chunk.begin(), chunk.end());
        }
    }

    return ReadStatus::kOk;
}

bool PartitionLog::Flush() { return active().segment->Sync(); }

bool PartitionLog::TruncateTo(int64_t offset) {
    if (offset < log_start_offset() || offset > log_end_offset_) {
        errno = EINVAL;
        return false;
    }

    if (offset == log_end_offset_) return true;  // Nothing to discard

    // Work out what to keep.
    auto keep = segments_.upper_bound(offset);
    keep--;
    if (keep->first == offset && keep != segments_.begin()) {
        keep--;
    }

    SegmentEntry& entry = keep->second;
    uint64_t cut = entry.segment->size();
    bool on_boundary = false;
    const OffsetIndex::Entry start = entry.index.Lookup(offset);
    const ScanResult r =
        entry.segment->Scan(start.position, start.offset, false,
                            [&](const BatchHeader& h, uint64_t p) {
                                if (h.base_offset >= offset) {
                                    on_boundary = (h.base_offset == offset);
                                    cut = p;
                                    return false;
                                }
                                return true;
                            });

    if (r.stop == ScanStop::kEndOfSegment) {
        on_boundary = (r.next_offset == offset);
    }
    if (r.stop == ScanStop::kIoError) return false;
    if (!on_boundary) {
        errno = EINVAL;  // offset inside of batch
        return false;
    }

    // Delete all later segments
    while (std::prev(segments_.end()) != keep) {
        auto last = std::prev(segments_.end());
        const std::string path = last->second.segment->path();
        segments_.erase(last);
        if (::unlink(path.c_str()) != 0) return false;
    }
    if (!SyncDir(dir_)) return false;

    // Cut the kept segment and fsync
    if (cut < entry.segment->size()) {
        if (!entry.segment->Truncate(cut) || !entry.segment->Sync()) {
            return false;
        }
        entry.index.TruncateTo(cut);
    }

    log_end_offset_ = offset;
    return true;
}

}  // namespace tributary
