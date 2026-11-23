#include <cstdint>
#include <initializer_list>
#include <vector>

#include "common/bytes.h"
#include "test.h"

namespace tributary {
namespace {

// Checks that 'actual' holds exactly the bytes in 'expected'
void check_bytes(const std::vector<uint8_t>& actual,
                 std::initializer_list<uint8_t> expected) {
    CHECK_EQ(actual.size(), expected.size());
    size_t i = 0;
    for (uint8_t want : expected) {
        if (i >= actual.size()) break;
        CHECK_EQ_HEX(actual[i], want);
        i++;
    }
}

/**
 * Test on-wire layout of every field type.
 */
void test_exact_bytes() {
    // u8
    {
        ByteWriter w;
        w.u8(0xAB);
        check_bytes(w.take(), {0xAB});
    }
    // u16
    {
        ByteWriter w;
        w.u16(0x0102);
        check_bytes(w.take(), {0x01, 0x02});
    }
    // u32
    {
        ByteWriter w;
        w.u32(0x01020304);
        check_bytes(w.take(), {0x01, 0x02, 0x03, 0x04});
    }
    // u64
    {
        ByteWriter w;
        w.u64(0x0102030405060708ull);
        check_bytes(w.take(), {
                                  0x01,
                                  0x02,
                                  0x03,
                                  0x04,
                                  0x05,
                                  0x06,
                                  0x07,
                                  0x08,
                              });
    }
    // i16 and i32
    {
        ByteWriter w;
        w.i16(-1);
        w.i32(-2);
        check_bytes(w.take(), {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE});
    }
    // str
    {
        ByteWriter w;
        w.str("ab");
        check_bytes(w.take(), {0x00, 0x02, 'a', 'b'});
    }
    // blob (includes bytes implicitly)
    {
        ByteWriter w;
        const uint8_t payload[] = {0x7F, 0x80};
        w.blob(payload, sizeof(payload));
        check_bytes(w.take(), {0x00, 0x00, 0x00, 0x02, 0x7F, 0x80});
    }
}

void test_unsigned_round_trip_edges() {
    ByteWriter w;
    w.u8(0);
    w.u8(std::numeric_limits<uint8_t>::max());
    w.u16(0);
    w.u16(std::numeric_limits<uint16_t>::max());
    w.u32(0);
    w.u32(std::numeric_limits<uint32_t>::max());
    w.u64(0);
    w.u64(std::numeric_limits<uint64_t>::max());
    const std::vector<uint8_t> buf = w.take();

    ByteReader r(buf);
    CHECK_EQ(r.u8(), uint8_t{0});
    CHECK_EQ(r.u8(), std::numeric_limits<uint8_t>::max());
    CHECK_EQ(r.u16(), uint16_t{0});
    CHECK_EQ(r.u16(), std::numeric_limits<uint16_t>::max());
    CHECK_EQ(r.u32(), 0u);
    CHECK_EQ(r.u32(), std::numeric_limits<uint32_t>::max());
    CHECK_EQ(r.u64(), uint64_t{0});
    CHECK_EQ(r.u64(), std::numeric_limits<uint64_t>::max());
    CHECK(r.ok());
    CHECK_EQ(r.remaining(), size_t{0});
}

void test_signed_round_trip_edges() {
    ByteWriter w;
    w.i16(-1);
    w.i16(std::numeric_limits<int16_t>::min());
    w.i16(std::numeric_limits<int16_t>::max());
    w.i32(-1);
    w.i32(std::numeric_limits<int32_t>::min());
    w.i32(std::numeric_limits<int32_t>::max());
    w.i64(-1);
    w.i64(std::numeric_limits<int64_t>::min());
    w.i64(std::numeric_limits<int64_t>::max());
    const std::vector<uint8_t> buf = w.take();

    ByteReader r(buf);
    CHECK_EQ(r.i16(), int16_t{-1});
    CHECK_EQ(r.i16(), std::numeric_limits<int16_t>::min());
    CHECK_EQ(r.i16(), std::numeric_limits<int16_t>::max());
    CHECK_EQ(r.i32(), -1);
    CHECK_EQ(r.i32(), std::numeric_limits<int32_t>::min());
    CHECK_EQ(r.i32(), std::numeric_limits<int32_t>::max());
    CHECK_EQ(r.i64(), int64_t{-1});
    CHECK_EQ(r.i64(), std::numeric_limits<int64_t>::min());
    CHECK_EQ(r.i64(), std::numeric_limits<int64_t>::max());
    CHECK(r.ok());
    CHECK_EQ(r.remaining(), size_t{0});
}

void test_strings_and_blobs() {
    const std::string long_str(1000, 'x');
    const std::string max_str(std::numeric_limits<uint16_t>::max(), 'y');
    const uint8_t payload[] = {1, 2, 3, 4, 5};

    ByteWriter w;
    w.str("");
    w.str("orders");
    w.str(long_str);
    w.str(max_str);
    w.blob(nullptr, 0);
    w.blob(payload, sizeof(payload));
    const std::vector<uint8_t> buf = w.take();

    ByteReader r(buf);
    CHECK_EQ(r.str(), std::string());
    CHECK_EQ(r.str(), std::string("orders"));
    CHECK(r.str() == long_str);
    CHECK(r.str() == max_str);

    const ByteView empty = r.blob();
    CHECK_EQ(empty.size, size_t{0});

    const ByteView view = r.blob();
    CHECK_EQ(view.size, sizeof(payload));
    for (size_t i = 0; i < view.size && i < sizeof(payload); ++i) {
        CHECK_EQ(view.data[i], payload[i]);
    }
    // The view points into `buf`, not a copy.
    CHECK(view.data >= buf.data() && view.data < buf.data() + buf.size());

    CHECK(r.ok());
    CHECK_EQ(r.remaining(), size_t{0});
}

// A realistic mix of field types, read back in order.
void test_mixed_sequence() {
    ByteWriter w;
    w.i64(500);       // base offset
    w.i32(3);         // leader epoch
    w.str("orders");  // topic
    w.i32(-1);        // replica id meaning "consumer"
    w.u8(1);
    const std::vector<uint8_t> buf = w.take();

    ByteReader r(buf);
    CHECK_EQ(r.i64(), int64_t{500});
    CHECK_EQ(r.i32(), 3);
    CHECK_EQ(r.str(), std::string("orders"));
    CHECK_EQ(r.i32(), -1);
    CHECK_EQ(r.u8(), uint8_t{1});
    CHECK(r.ok());
    CHECK_EQ(r.position(), buf.size());
    CHECK_EQ(r.remaining(), size_t{0});
}

// A single short read fails without moving, and the failure is sticky.
void test_truncated_primitive_is_sticky() {
    const uint8_t data[] = {0x01, 0x02, 0x03};
    ByteReader r(data, sizeof(data));

    CHECK_EQ(r.u32(), 0u);
    CHECK(!r.ok());
    CHECK_EQ(r.position(), size_t{0});

    // Enough bytes remain for a u8, but the reader stays failed.
    CHECK_EQ(r.u8(), uint8_t{0});
    CHECK_EQ(r.u16(), uint16_t{0});
    CHECK(r.bytes(1) == nullptr);
    CHECK_EQ(r.str(), std::string());
    CHECK(!r.ok());
    CHECK_EQ(r.position(), size_t{0});
}

// Every width fails on a buffer one byte too short.
void test_each_width_fails_one_short() {
    const uint8_t data[8] = {};
    {
        ByteReader r(data, 0);
        r.u8();
        CHECK(!r.ok());
    }
    {
        ByteReader r(data, 1);
        r.u16();
        CHECK(!r.ok());
    }
    {
        ByteReader r(data, 3);
        r.i32();
        CHECK(!r.ok());
    }
    {
        ByteReader r(data, 7);
        r.i64();
        CHECK(!r.ok());
    }
    {
        ByteReader r(data, 8);
        r.u64();
        CHECK(r.ok());  // exactly enough is fine
    }
}

// A corrupt length prefix far larger than the buffer must fail cleanly.
// This is the case the overflow-safe bounds check exists for.
void test_huge_length_prefix() {
    {
        const uint8_t data[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x02};
        ByteReader r(data, sizeof(data));
        const ByteView v = r.blob();
        CHECK(v.data == nullptr);
        CHECK_EQ(v.size, size_t{0});
        CHECK(!r.ok());
    }
    {
        const uint8_t data[] = {0xFF, 0xFF, 'a', 'b'};
        ByteReader r(data, sizeof(data));
        CHECK_EQ(r.str(), std::string());
        CHECK(!r.ok());
    }
    {
        const uint8_t data[] = {0x00};
        ByteReader r(data, sizeof(data));
        r.skip(static_cast<size_t>(-1));
        CHECK(!r.ok());
    }
}

void test_bytes_and_skip() {
    const uint8_t data[] = {10, 20, 30, 40, 50};
    ByteReader r(data, sizeof(data));

    r.skip(1);
    CHECK_EQ(r.position(), size_t{1});

    const uint8_t* p = r.bytes(3);
    CHECK(p == data + 1);
    CHECK_EQ(r.position(), size_t{4});

    // Zero bytes succeeds and returns the current position.
    CHECK(r.bytes(0) == data + 4);
    CHECK(r.ok());

    CHECK_EQ(r.u8(), uint8_t{50});
    CHECK_EQ(r.remaining(), size_t{0});
    CHECK(r.ok());
}

// Placeholder, then body, then backfill: how batch encoding fills in
// its length and checksum.
void test_put_u32_at() {
    ByteWriter w;
    w.u8(0xAA);
    const size_t placeholder = w.size();
    w.u32(0);
    w.u16(0xBBCC);
    w.put_u32_at(placeholder, 0xDEADBEEF);

    CHECK_EQ(w.size(), size_t{7});  // overwriting does not grow the buffer
    check_bytes(w.take(), {0xAA, 0xDE, 0xAD, 0xBE, 0xEF, 0xBB, 0xCC});
}

void test_take_empties_writer() {
    ByteWriter w(64);
    w.u32(7);
    const std::vector<uint8_t> first = w.take();
    CHECK_EQ(first.size(), size_t{4});
    CHECK_EQ(w.size(), size_t{0});

    // The writer is reusable after take().
    w.u8(9);
    check_bytes(w.take(), {9});
}

}  // namespace
}  // namespace tributary

int main() {
    tributary::test_exact_bytes();
    tributary::test_unsigned_round_trip_edges();
    tributary::test_signed_round_trip_edges();
    tributary::test_strings_and_blobs();
    tributary::test_mixed_sequence();
    tributary::test_truncated_primitive_is_sticky();
    tributary::test_each_width_fails_one_short();
    tributary::test_huge_length_prefix();
    tributary::test_bytes_and_skip();
    tributary::test_put_u32_at();
    tributary::test_take_empties_writer();
    return tributary::ReportResults();
}
