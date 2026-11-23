/** Record batch format
 * ===================
 *
 * A batch is the unit that is stored, replicated and fetched. All integers
 * are big-endian. Producers encode batches; leaders stamp base_offset and
 * leader_epoch in place before appending; followers and consumers copy and
 * read them unchanged.
 *
 * Header (24 bytes)
 *
 *   Bytes   Field          Type   Notes
 *   -----   ------------   ----   ---------------------------------------------
 *    0-7    base_offset    i64    Offset of the first record. Set by the leader
 *    8-11   batch_length   i32    Bytes after this field: total size is
 *                                 12 + batch_length.
 *   12-15   leader_epoch   i32    Epoch of the leader that appended the batch.
 *                                 Set by the leader.
 *   16-19   crc            u32    CRC32C of bytes 20 to the end of the batch.
 *   20-23   record_count   i32    Number of records; at least 1.
 *   24-     records               record_count records, back to back.
 *
 * Record
 *
 *   Field          Type   Notes
 *   ------------   ----   ---------------------------------------------------
 *   offset_delta   u32    Position in the batch: 0, 1, 2, ...
 *                         The record's offset is base_offset + offset_delta.
 *   key_length     u32    0 means the record has no key.
 *   key            bytes  key_length bytes.
 *   value_length   u32
 *   value          bytes  value_length bytes.
 *
 * CRC coverage
 *
 *   The CRC covers record_count and every record. It does not cover
 *   base_offset, batch_length or leader_epoch, so the leader can stamp
 *   base_offset and leader_epoch without recomputing it. Readers check
 *   those uncovered fields separately: batch_length must fit within the
 *   available bytes and kMaxBatchSize, and base_offset must continue
 *   from the previous batch in the log.
 *
 * Validity
 *
 *   A batch is valid when its total size is at most kMaxBatchSize, its
 *   record_count is at least 1, its CRC matches, and its records fill
 *   exactly batch_length bytes.
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tributary {

struct Record {
    int64_t offset = -1;  // set by DecodeRecords, ignored by EncodeBatch
    std::string key;      // can be empty
    std::string value;
};

struct BatchHeader {
    int64_t base_offset;
    int32_t batch_length;
    int32_t leader_epoch;
    uint32_t crc;
    int32_t record_count;

    size_t total_size() const { return 12 + static_cast<size_t>(batch_length); }
    int64_t last_offset() const { return base_offset + record_count - 1; }
    int64_t next_offset() const { return base_offset + record_count; }
};

enum class BatchStatus { kOk, kIncomplete, kCorrupt };

constexpr size_t kBaseOffsetPos = 0;    // i64, stamped by leader
constexpr size_t kBatchLengthPos = 8;   // i32, bytes after this field
constexpr size_t kLeaderEpochPos = 12;  // i32, stamped by leader
constexpr size_t kCrcPos = 16;          // u32
constexpr size_t kCrcStart = 20;        // CRC covers record_count onward
constexpr size_t kLengthFieldEnd = 12;  // total size = 12 + batch_length
constexpr size_t kBatchHeaderSize = 24;
constexpr size_t kMinBatchLength = kBatchHeaderSize - kLengthFieldEnd;
constexpr size_t kMaxBatchSize = 16 * 1024 * 1024;

/**
 * Producer side; base_offset and leader_epoch are written as 0.
 *
 * Returns empty if records is empty or if batch would exceed kMaxBatchSize.
 */
std::vector<uint8_t> EncodeBatch(const std::vector<Record>& records);

/**
 * Leader side; fills in fields the base_offset and leader_epoch in-place.
 * batch must point to a complete batch that passed CheckBatch.
 *
 * base_offset and leader_epoch are not covered by the checksum.
 */
void StampBatch(uint8_t* batch, int64_t base_offset, int32_t leader_epoch);

/**
 * Reads header at data[0..avail) and validates the entire batch.
 */
BatchStatus CheckBatch(const uint8_t* data, size_t avail, BatchHeader* out);

/**
 * Header only; skips CRC check for scanning past trusted batches.
 */
BatchStatus ReadBatchHeader(const uint8_t* data, size_t avail,
                            BatchHeader* out);

/**
 * Consumer side; decode the records of a checked batch.
 */
bool DecodeRecords(const uint8_t* batch, size_t size, std::vector<Record>* out);

}  // namespace tributary
