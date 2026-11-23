#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "common/bytes.h"
#include "common/crc32c.h"
#include "storage/record_batch.h"
#include "test.h"

namespace tributary {

// Lets CHECK_EQ print a BatchStatus when a check fails. It lives directly in
// namespace tributary (not the unnamed namespace below) so that argument-
// dependent lookup finds it from inside test.h's PrintValue.
std::ostream& operator<<(std::ostream& os, BatchStatus s) {
    switch (s) {
        case BatchStatus::kOk:
            return os << "kOk";
        case BatchStatus::kIncomplete:
            return os << "kIncomplete";
        case BatchStatus::kCorrupt:
            return os << "kCorrupt";
    }
    return os << "BatchStatus(" << static_cast<int>(s) << ")";
}

namespace {

// Field positions written out as literals, straight from the layout table in
// record_batch.h. The tests deliberately don't use the library's constants,
// so they check the documented format rather than agree with the code.
constexpr size_t kPosBaseOffset = 0;
constexpr size_t kPosBatchLength = 8;
constexpr size_t kPosLeaderEpoch = 12;
constexpr size_t kPosCrc = 16;
constexpr size_t kPosRecordCount = 20;
constexpr size_t kPosRecords = 24;

Record rec(const std::string& key, const std::string& value) {
    Record r;
    r.key = key;
    r.value = value;
    return r;
}

uint32_t load_be32(const std::vector<uint8_t>& b, size_t pos) {
    return (uint32_t{b[pos]} << 24) | (uint32_t{b[pos + 1]} << 16) |
           (uint32_t{b[pos + 2]} << 8) | uint32_t{b[pos + 3]};
}

uint64_t load_be64(const std::vector<uint8_t>& b, size_t pos) {
    return (uint64_t{load_be32(b, pos)} << 32) | load_be32(b, pos + 4);
}

// A record for hand-built batches, where the delta can be anything.
struct RawRecord {
    uint32_t delta;
    std::string key;
    std::string value;
};

// Builds a batch by hand with a correct length and CRC but whatever
// record_count, deltas and trailing bytes the test asks for. EncodeBatch
// can't produce these mistakes, which is the point.
std::vector<uint8_t> raw_batch(int32_t record_count,
                               const std::vector<RawRecord>& records,
                               size_t trailing_bytes = 0) {
    ByteWriter w;
    w.i64(0);
    w.i32(0);
    w.i32(0);
    w.u32(0);
    w.i32(record_count);
    for (const RawRecord& r : records) {
        w.u32(r.delta);
        w.blob(reinterpret_cast<const uint8_t*>(r.key.data()), r.key.size());
        w.blob(reinterpret_cast<const uint8_t*>(r.value.data()),
               r.value.size());
    }
    for (size_t i = 0; i < trailing_bytes; ++i) w.u8(0xEE);

    const size_t total = w.size();
    w.put_u32_at(kPosBatchLength, static_cast<uint32_t>(total - 12));
    w.put_u32_at(kPosCrc,
                 crc32c(w.data() + kPosRecordCount, total - kPosRecordCount));
    return w.take();
}

// Overwrites a big-endian u32 field in place (used to plant bad headers).
void set_be32(std::vector<uint8_t>* b, size_t pos, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) {
        (*b)[pos + i] = static_cast<uint8_t>(v >> (8 * (3 - i)));
    }
}

// ---------------------------------------------------------------------------
// Encoding and the on-disk layout

void test_round_trip() {
    const std::string big(5000, 'v');
    std::vector<uint8_t> b = EncodeBatch({
        rec("alice", "order-1"),
        rec("", "no key"),
        rec("bob", ""),
        rec("alice", big),
    });
    CHECK(!b.empty());
    StampBatch(b.data(), 500, 3);

    BatchHeader h{};
    CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kOk);
    CHECK_EQ(h.base_offset, int64_t{500});
    CHECK_EQ(h.leader_epoch, 3);
    CHECK_EQ(h.record_count, 4);
    CHECK_EQ(h.total_size(), b.size());

    std::vector<Record> out;
    CHECK(DecodeRecords(b.data(), b.size(), &out));
    CHECK_EQ(out.size(), size_t{4});
    if (out.size() != 4) return;

    CHECK_EQ(out[0].offset, int64_t{500});
    CHECK_EQ(out[0].key, std::string("alice"));
    CHECK_EQ(out[0].value, std::string("order-1"));
    CHECK_EQ(out[1].offset, int64_t{501});
    CHECK(out[1].key.empty());
    CHECK_EQ(out[1].value, std::string("no key"));
    CHECK_EQ(out[2].offset, int64_t{502});
    CHECK_EQ(out[2].key, std::string("bob"));
    CHECK(out[2].value.empty());
    CHECK_EQ(out[3].offset, int64_t{503});
    CHECK(out[3].value == big);
}

void test_single_record() {
    std::vector<uint8_t> b = EncodeBatch({rec("k", "v")});
    BatchHeader h{};
    CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kOk);
    CHECK_EQ(h.record_count, 1);
    CHECK_EQ(h.last_offset(), int64_t{0});
    CHECK_EQ(h.next_offset(), int64_t{1});
}

// Pins every header field and the first record to its documented position.
void test_exact_layout() {
    std::vector<uint8_t> b = EncodeBatch({rec("k", "v"), rec("k", "v")});

    // 24-byte header + 2 records of (4 + 4 + 1 + 4 + 1) bytes.
    CHECK_EQ(b.size(), size_t{52});
    if (b.size() != 52) return;

    CHECK_EQ_HEX(load_be64(b, kPosBaseOffset), uint64_t{0});
    CHECK_EQ_HEX(load_be32(b, kPosBatchLength), 40u);  // 52 - 12
    CHECK_EQ_HEX(load_be32(b, kPosLeaderEpoch), 0u);
    CHECK_EQ_HEX(load_be32(b, kPosCrc), crc32c(b.data() + kPosRecordCount,
                                               b.size() - kPosRecordCount));
    CHECK_EQ_HEX(load_be32(b, kPosRecordCount), 2u);

    // First record: delta 0, key "k", value "v".
    CHECK_EQ_HEX(load_be32(b, kPosRecords), 0u);
    CHECK_EQ_HEX(load_be32(b, kPosRecords + 4), 1u);
    CHECK_EQ_HEX(b[kPosRecords + 8], uint8_t{'k'});
    CHECK_EQ_HEX(load_be32(b, kPosRecords + 9), 1u);
    CHECK_EQ_HEX(b[kPosRecords + 13], uint8_t{'v'});
    // Second record starts right after, with delta 1.
    CHECK_EQ_HEX(load_be32(b, kPosRecords + 14), 1u);

    // Stamping writes only base_offset and leader_epoch.
    std::vector<uint8_t> stamped = b;
    StampBatch(stamped.data(), 500, 3);
    CHECK_EQ_HEX(load_be64(stamped, kPosBaseOffset), uint64_t{500});
    CHECK_EQ_HEX(load_be32(stamped, kPosLeaderEpoch), 3u);
    for (size_t i = 0; i < b.size(); ++i) {
        const bool stamped_field = i < 8 || (i >= 12 && i < 16);
        if (!stamped_field) CHECK_EQ_HEX(stamped[i], b[i]);
    }
}

void test_header_helpers() {
    BatchHeader h{};
    h.base_offset = 500;
    h.batch_length = 40;
    h.record_count = 3;
    CHECK_EQ(h.total_size(), size_t{52});
    CHECK_EQ(h.last_offset(), int64_t{502});
    CHECK_EQ(h.next_offset(), int64_t{503});
}

void test_stamping_keeps_crc_valid() {
    std::vector<uint8_t> b = EncodeBatch({rec("a", "1"), rec("b", "2")});
    BatchHeader h{};

    StampBatch(b.data(), 1000, 7);
    CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kOk);
    CHECK_EQ(h.base_offset, int64_t{1000});
    CHECK_EQ(h.leader_epoch, 7);

    // Re-stamping (a later leader) is also fine.
    StampBatch(b.data(), 2000, 8);
    CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kOk);
    CHECK_EQ(h.base_offset, int64_t{2000});
}

void test_encode_refuses_empty_and_oversized() {
    CHECK(EncodeBatch({}).empty());

    // Exactly at the limit: header + one record whose value fills the rest.
    const size_t fits = kMaxBatchSize - kBatchHeaderSize - 12;
    std::vector<uint8_t> max_batch =
        EncodeBatch({rec("", std::string(fits, 'x'))});
    CHECK_EQ(max_batch.size(), kMaxBatchSize);
    BatchHeader h{};
    CHECK_EQ(CheckBatch(max_batch.data(), max_batch.size(), &h),
             BatchStatus::kOk);

    // One byte more is refused.
    CHECK(EncodeBatch({rec("", std::string(fits + 1, 'x'))}).empty());
}

// ---------------------------------------------------------------------------
// Detecting damage

// Every byte the CRC covers is protected.
void test_flipped_covered_byte_is_corrupt() {
    const std::vector<uint8_t> good =
        EncodeBatch({rec("alice", "order-1"), rec("bob", "order-2")});
    BatchHeader h{};
    for (size_t i = kPosRecordCount; i < good.size(); ++i) {
        std::vector<uint8_t> b = good;
        b[i] ^= 0x01;
        CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kCorrupt);
    }
}

// A crash can cut a batch off anywhere; every prefix is incomplete.
void test_every_prefix_is_incomplete() {
    const std::vector<uint8_t> b =
        EncodeBatch({rec("alice", "order-1"), rec("bob", "order-2")});
    BatchHeader h{};
    for (size_t len = 0; len < b.size(); ++len) {
        CHECK_EQ(CheckBatch(b.data(), len, &h), BatchStatus::kIncomplete);
    }
    CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kOk);
}

// ReadBatchHeader needs only the header; CheckBatch needs everything.
void test_header_only_read() {
    const std::vector<uint8_t> b = EncodeBatch({rec("alice", "order-1")});
    BatchHeader h{};
    CHECK_EQ(ReadBatchHeader(b.data(), kBatchHeaderSize, &h), BatchStatus::kOk);
    CHECK_EQ(h.total_size(), b.size());
    CHECK_EQ(CheckBatch(b.data(), kBatchHeaderSize, &h),
             BatchStatus::kIncomplete);
    CHECK_EQ(ReadBatchHeader(b.data(), kBatchHeaderSize - 1, &h),
             BatchStatus::kIncomplete);
}

// Nonsense headers are corrupt, never "incomplete", even with plenty of
// bytes missing. Otherwise a reader could wait forever for bytes that
// will never come.
void test_rejects_nonsense_headers() {
    const std::vector<uint8_t> good = EncodeBatch({rec("alice", "order-1")});
    BatchHeader h{};

    {  // batch_length far beyond kMaxBatchSize
        std::vector<uint8_t> b = good;
        set_be32(&b, kPosBatchLength, 0x7FFFFFFF);
        CHECK_EQ(ReadBatchHeader(b.data(), b.size(), &h),
                 BatchStatus::kCorrupt);
        CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kCorrupt);
    }
    {  // batch_length too small to hold epoch + CRC + count
        std::vector<uint8_t> b = good;
        set_be32(&b, kPosBatchLength, 11);
        CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kCorrupt);
    }
    {  // negative batch_length
        std::vector<uint8_t> b = good;
        set_be32(&b, kPosBatchLength, 0xFFFFFFFF);
        CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kCorrupt);
    }
    {  // negative base_offset
        std::vector<uint8_t> b = good;
        StampBatch(b.data(), -1, 0);
        CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kCorrupt);
    }
}

// record_count values that can't be right, each with a valid CRC, so only
// the header checks can catch them.
void test_rejects_impossible_record_counts() {
    BatchHeader h{};

    {  // zero records
        std::vector<uint8_t> b = raw_batch(0, {});
        CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kCorrupt);
    }
    {  // negative
        std::vector<uint8_t> b = raw_batch(-1, {{0, "", ""}});
        CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kCorrupt);
    }
    {  // far more records than the body has room for. Without the
        // record_count bound, DecodeRecords would try to reserve ~150 GB.
        std::vector<uint8_t> b =
            raw_batch(0x7FFFFFFF, {{0, "", ""}, {1, "", ""}});
        CHECK_EQ(ReadBatchHeader(b.data(), b.size(), &h),
                 BatchStatus::kCorrupt);
        CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kCorrupt);
        std::vector<Record> out;
        CHECK(!DecodeRecords(b.data(), b.size(), &out));
        CHECK(out.empty());
    }
}

// ---------------------------------------------------------------------------
// Several batches in one buffer, as in a segment or a fetch response

void test_back_to_back_batches() {
    std::vector<uint8_t> first =
        EncodeBatch({rec("a", "1"), rec("b", "2"), rec("c", "3")});
    std::vector<uint8_t> second = EncodeBatch({rec("d", "4")});
    StampBatch(first.data(), 500, 1);
    StampBatch(second.data(), 503, 1);

    std::vector<uint8_t> log = first;
    log.insert(log.end(), second.begin(), second.end());

    BatchHeader h{};
    size_t pos = 0;
    CHECK_EQ(CheckBatch(log.data() + pos, log.size() - pos, &h),
             BatchStatus::kOk);
    CHECK_EQ(h.base_offset, int64_t{500});
    CHECK_EQ(h.next_offset(), int64_t{503});

    pos += h.total_size();
    CHECK_EQ(CheckBatch(log.data() + pos, log.size() - pos, &h),
             BatchStatus::kOk);
    CHECK_EQ(h.base_offset, int64_t{503});

    pos += h.total_size();
    CHECK_EQ(pos, log.size());
    CHECK_EQ(CheckBatch(log.data() + pos, log.size() - pos, &h),
             BatchStatus::kIncomplete);
}

// ---------------------------------------------------------------------------
// Decoding

void test_decode_appends() {
    std::vector<uint8_t> b = EncodeBatch({rec("x", "1"), rec("y", "2")});
    StampBatch(b.data(), 10, 0);

    std::vector<Record> out;
    out.push_back(rec("existing", "record"));
    CHECK(DecodeRecords(b.data(), b.size(), &out));
    CHECK_EQ(out.size(), size_t{3});
    if (out.size() == 3) {
        CHECK_EQ(out[0].key, std::string("existing"));
        CHECK_EQ(out[1].offset, int64_t{10});
        CHECK_EQ(out[2].offset, int64_t{11});
    }
}

void test_decode_rejects_short_buffer() {
    const std::vector<uint8_t> b = EncodeBatch({rec("alice", "order-1")});
    std::vector<Record> out;
    CHECK(!DecodeRecords(b.data(), b.size() - 1, &out));
    CHECK(out.empty());
}

// Records must fill batch_length exactly. These batches pass CheckBatch
// (their CRCs are valid), so only DecodeRecords can catch them.
void test_decode_rejects_inconsistent_records() {
    BatchHeader h{};

    {  // count says 1, but 2 records follow: leftover bytes
        std::vector<uint8_t> b = raw_batch(1, {{0, "a", "1"}, {1, "b", "2"}});
        CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kOk);
        std::vector<Record> out;
        CHECK(!DecodeRecords(b.data(), b.size(), &out));
    }
    {  // count says 2, but the bytes after the first record aren't a record
        std::vector<uint8_t> b = raw_batch(2, {{0, "a", "1"}}, 12);
        CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kOk);
        std::vector<Record> out;
        CHECK(!DecodeRecords(b.data(), b.size(), &out));
    }
    {  // trailing garbage after the last record
        std::vector<uint8_t> b = raw_batch(1, {{0, "a", "1"}}, 3);
        CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kOk);
        std::vector<Record> out;
        CHECK(!DecodeRecords(b.data(), b.size(), &out));
    }
    {  // deltas out of order
        std::vector<uint8_t> b = raw_batch(2, {{0, "a", "1"}, {5, "b", "2"}});
        CHECK_EQ(CheckBatch(b.data(), b.size(), &h), BatchStatus::kOk);
        std::vector<Record> out;
        CHECK(!DecodeRecords(b.data(), b.size(), &out));
    }
    {  // a well-formed hand-built batch decodes, so the cases above fail
        // for the reason they test and not because raw_batch is wrong
        std::vector<uint8_t> b = raw_batch(2, {{0, "a", "1"}, {1, "b", "2"}});
        std::vector<Record> out;
        CHECK(DecodeRecords(b.data(), b.size(), &out));
        CHECK_EQ(out.size(), size_t{2});
    }
}

// A failed decode leaves the caller's vector exactly as it was.
void test_decode_failure_leaves_out_unchanged() {
    std::vector<uint8_t> b = raw_batch(2, {{0, "a", "1"}, {9, "b", "2"}});
    std::vector<Record> out;
    out.push_back(rec("keep", "me"));
    CHECK(!DecodeRecords(b.data(), b.size(), &out));
    CHECK_EQ(out.size(), size_t{1});
    if (!out.empty()) CHECK_EQ(out[0].key, std::string("keep"));
}

}  // namespace
}  // namespace tributary

int main() {
    tributary::test_round_trip();
    tributary::test_single_record();
    tributary::test_exact_layout();
    tributary::test_header_helpers();
    tributary::test_stamping_keeps_crc_valid();
    tributary::test_encode_refuses_empty_and_oversized();
    tributary::test_flipped_covered_byte_is_corrupt();
    tributary::test_every_prefix_is_incomplete();
    tributary::test_header_only_read();
    tributary::test_rejects_nonsense_headers();
    tributary::test_rejects_impossible_record_counts();
    tributary::test_back_to_back_batches();
    tributary::test_decode_appends();
    tributary::test_decode_rejects_short_buffer();
    tributary::test_decode_rejects_inconsistent_records();
    tributary::test_decode_failure_leaves_out_unchanged();
    return tributary::ReportResults();
}
