/**
 * A sparse offset index for one segment.
 *
 * I had originally planned to follow Kafka and use .index files, but this
 * version lives in-memory.
 *
 * The index maps the base offsets of some batches to their byte positions in
 * the segment file, about one entry every 'interval_bytes'.  Finding offset X
 * is done by binary search, followed by a short forward scan of batch headers
 * in the file, rather than a scan from the start of the segment.
 *
 * NOT thread-safe. The caller (partition) holds the lock.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tributary {

/**
 * Maps the base offsets of some batches to their byte positions in *the segment
 * file,
 * about one entry every 'interval_bytes'.Finding offset X *is done by binary
 * search,
 * followed by a short forward scan of batch headers *in the file, rather than a
 * scan from the start of the segment.
 *
 * NOT thread - safe.The caller(partition) holds the lock.
 */
class OffsetIndex {
   public:
    /**
     * One index batch.
     */
    struct Entry {
        int64_t offset;
        uint64_t position;
    };

    /**
     * Creates an empty index for the segment whose first offset is
     * segment_base_offset. An entry is kept roughly every interval_bytes bytes
     * of the log.
     */
    explicit OffsetIndex(int64_t segment_base_offset,
                         uint32_t interval_bytes = 4096);

    /**
     * Records a batch if it is due an entry.
     *
     * Should be called once per-batch in log order with the batch's base_offset
     * and the position where it starts. The first batch always get an entry,
     * later ones only once interval_bytes bytes have passed since the last
     * entry. Offsets and positions should increase from call to call.
     */
    void MaybeAdd(int64_t batch_base_offset, uint64_t position);

    /**
     * Returns the best place to start looking for 'offset'.
     *
     * The last entry whose offset is at or before 'offset'. If there are none
     * (empty index, offset comes before every entry), it returns the starting
     * segment {segment base offset, 0}. Scanning forward from the result is
     * guaranteed to reach the batch holding 'offset', if it is in this segment.
     */
    Entry Lookup(int64_t offset) const;

    /**
     * Drops every entry at or before byte 'position'.
     *
     * Should be called after the segment file is truncated to 'position', so
     * that the index never points past the end of the log.
     */
    void TruncateTo(uint64_t position);

    /**
     * Number of entries.
     */
    size_t size() const { return entries_.size(); }

    /**
     * Returns true if no batch has yet been recorded.
     */
    bool empty() const { return entries_.empty(); }

    int64_t segment_base_offset() const { return segment_base_offset_; }
    uint32_t interval_bytes() const { return interval_bytes_; }

   private:
    std::vector<Entry> entries_;
    int64_t segment_base_offset_;
    uint32_t interval_bytes_;
};

}  // namespace tributary
