#include "record_batch.h"

#include <cstddef>
#include <iterator>

#include "common/bytes.h"
#include "common/crc32c.h"

namespace tributary {
namespace {

/**
 * Reinterprets a string as bytes.
 *
 * Mostly so I don't have to type `reinterpret_cast...` every single time.
 */
const uint8_t* as_bytes(const std::string& s) {
    return reinterpret_cast<const uint8_t*>(s.data());
}

std::string as_string(ByteView v) {
    return std::string(reinterpret_cast<const char*>(v.data), v.size);
}

}  // namespace

std::vector<uint8_t> EncodeBatch(const std::vector<Record>& records) {
    if (records.empty()) return {};

    // Check the final size (header + records) is within max length
    size_t encoded_size = kBatchHeaderSize;
    for (const Record& r : records) {
        encoded_size += 12 + r.key.size() + r.value.size();
    }

    if (encoded_size > kMaxBatchSize) return {};

    ByteWriter writer(encoded_size);

    // Header. Length and CRC are backfilled, base_offset and leader_epoch are
    // stamped by the leader
    writer.i64(0);                                     // base_offset
    writer.i32(0);                                     // batch_length
    writer.i32(0);                                     // leader_epoch
    writer.u32(0);                                     // crc
    writer.i32(static_cast<int32_t>(records.size()));  // record_count

    // Records
    for (size_t i = 0; i < records.size(); i++) {
        const Record& r = records[i];
        writer.u32(static_cast<uint32_t>(i));            // offset_delta
        writer.blob(as_bytes(r.key), r.key.size());      // key
        writer.blob(as_bytes(r.value), r.value.size());  // value
    }

    // Backfill the length and CRC
    const size_t total = writer.size();
    writer.put_u32_at(kBatchLengthPos,
                      static_cast<uint32_t>(total - kLengthFieldEnd));
    writer.put_u32_at(kCrcPos,
                      crc32c(writer.data() + kCrcStart, total - kCrcStart));

    return writer.take();
}

void StampBatch(uint8_t* batch, int64_t base_offset, int32_t leader_epoch) {
    detail::store_be(batch + kBaseOffsetPos,
                     static_cast<uint64_t>(base_offset));
    detail::store_be(batch + kLeaderEpochPos,
                     static_cast<uint32_t>(leader_epoch));
}

BatchStatus CheckBatch(const uint8_t* data, size_t avail, BatchHeader* out) {
    BatchStatus head_status = ReadBatchHeader(data, avail, out);
    if (head_status != BatchStatus::kOk) return head_status;

    if (out->total_size() > avail) return BatchStatus::kIncomplete;
    if (crc32c(data + kCrcStart, out->total_size() - kCrcStart) != out->crc) {
        return BatchStatus::kCorrupt;
    }

    return BatchStatus::kOk;
}

BatchStatus ReadBatchHeader(const uint8_t* data, size_t avail,
                            BatchHeader* out) {
    if (avail < kBatchHeaderSize) return BatchStatus::kIncomplete;
    ByteReader reader(data, avail);

    out->base_offset = reader.i64();
    out->batch_length = reader.i32();
    out->leader_epoch = reader.i32();
    out->crc = reader.u32();
    out->record_count = reader.i32();

    constexpr size_t kMinRecordSize = 12;

    if (out->batch_length < static_cast<int32_t>(kMinBatchLength))
        return BatchStatus::kCorrupt;
    if (out->total_size() > kMaxBatchSize) return BatchStatus::kCorrupt;
    if (out->record_count < 1) return BatchStatus::kCorrupt;
    if (static_cast<size_t>(out->record_count) >
        (out->total_size() - kBatchHeaderSize) / kMinRecordSize) {
        return BatchStatus::kCorrupt;
    }
    if (out->base_offset < 0) return BatchStatus::kCorrupt;
    if (out->leader_epoch < 0) return BatchStatus::kCorrupt;

    return BatchStatus::kOk;
}

bool DecodeRecords(const uint8_t* batch, size_t size,
                   std::vector<Record>* out) {
    BatchHeader header;
    if (ReadBatchHeader(batch, size, &header) != BatchStatus::kOk) return false;
    if (size < header.total_size()) return false;

    ByteReader reader(batch + kBatchHeaderSize,
                      header.total_size() - kBatchHeaderSize);

    std::vector<Record> decoded;
    decoded.reserve(header.record_count);

    for (int32_t i = 0; i < header.record_count; i++) {
        uint32_t offset_delta = reader.u32();
        ByteView key = reader.blob();
        ByteView value = reader.blob();

        if (!reader.ok()) return false;
        if (offset_delta != static_cast<uint32_t>(i)) return false;

        decoded.push_back(Record{header.base_offset + offset_delta,
                                 as_string(key), as_string(value)});
    }

    // Records should fill batch exactly
    if (reader.remaining() != 0) {
        return false;
    }

    out->insert(out->end(), std::make_move_iterator(decoded.begin()),
                std::make_move_iterator(decoded.end()));

    return true;
}

}  // namespace tributary
