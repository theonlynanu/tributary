#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#include "storage/record_batch.h"
#include "storage/segment.h"
#include "test.h"
#include "test_util.h"

namespace tributary {

namespace {

// A stamped batch of `count` records starting at `base_offset`.
std::vector<uint8_t> batch_at(int64_t base_offset, int count) {
    std::vector<Record> records(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        records[static_cast<size_t>(i)].key = "k";
        records[static_cast<size_t>(i)].value =
            "value-" + std::to_string(base_offset + i);
    }
    std::vector<uint8_t> b = EncodeBatch(records);
    StampBatch(b.data(), base_offset, 0);
    return b;
}

// Where each batch landed, as recorded by a scan.
struct Seen {
    int64_t offset;
    uint64_t position;
};

// Scans the whole segment, collecting every accepted batch.
ScanResult scan_all(const Segment& seg, bool verify_crc,
                    std::vector<Seen>* seen) {
    return seg.Scan(0, seg.base_offset(), verify_crc,
                    [seen](const BatchHeader& h, uint64_t pos) {
                        seen->push_back({h.base_offset, pos});
                        return true;
                    });
}

// Fills a segment at base 0 with batches of 3, 2 and 1 records (offsets
// 0, 3 and 5), returning each batch's start position and the total size.
struct Layout {
    std::vector<uint64_t> starts;
    uint64_t total;
};

Layout write_three_batches(const std::string& dir) {
    auto seg = Segment::Create(dir, 0);
    Layout layout{{}, 0};
    for (const auto& b : {batch_at(0, 3), batch_at(3, 2), batch_at(5, 1)}) {
        layout.starts.push_back(seg->size());
        CHECK(seg->Append(b.data(), b.size()));
    }
    layout.total = seg->size();
    CHECK(seg->Sync());
    return layout;
}  // segment closed here, as if the process exited

// ---------------------------------------------------------------------------
// File names

void test_file_names() {
    CHECK_EQ(SegmentFileName(0), std::string("00000000000000000000.log"));
    CHECK_EQ(SegmentFileName(500), std::string("00000000000000000500.log"));
    CHECK_EQ(SegmentFileName(9223372036854775807),
             std::string("09223372036854775807.log"));

    int64_t base = -1;
    CHECK(ParseSegmentFileName("00000000000000000500.log", &base));
    CHECK_EQ(base, int64_t{500});
    CHECK(ParseSegmentFileName(SegmentFileName(9223372036854775807), &base));
    CHECK_EQ(base, int64_t{9223372036854775807});

    const char* rejected[] = {
        "500.log",                   // not padded
        "00000000000000000500.tmp",  // wrong suffix
        "00000000000000000500.log.tmp",
        "0000000000000000050a.log",  // not all digits
        "-0000000000000000500.log",
        "99999999999999999999.log",  // too big for int64
        "state",
        "",
    };
    for (const char* name : rejected) {
        CHECK(!ParseSegmentFileName(name, &base));
    }
}

// ---------------------------------------------------------------------------
// Creating and opening

void test_create_and_open() {
    TempDir dir;

    auto seg = Segment::Create(dir.path(), 500);
    CHECK(seg != nullptr);
    if (!seg) return;
    CHECK_EQ(seg->base_offset(), int64_t{500});
    CHECK_EQ(seg->size(), uint64_t{0});
    CHECK_EQ(seg->path(), dir.file("00000000000000000500.log"));
    CHECK(ListFiles(dir.path()) ==
          std::vector<std::string>{"00000000000000000500.log"});

    // Creating the same segment again is refused.
    CHECK(Segment::Create(dir.path(), 500) == nullptr);
    CHECK_EQ(errno, EEXIST);

    // Opening a segment that doesn't exist is refused.
    CHECK(Segment::OpenExisting(dir.path(), 600) == nullptr);
    CHECK_EQ(errno, ENOENT);

    // Opening an existing one works.
    CHECK(Segment::OpenExisting(dir.path(), 500) != nullptr);
}

// ---------------------------------------------------------------------------
// Appending and reading

void test_append_and_read_back() {
    TempDir dir;
    auto seg = Segment::Create(dir.path(), 0);

    const auto a = batch_at(0, 3);
    const auto b = batch_at(3, 2);
    CHECK(seg->Append(a.data(), a.size()));
    CHECK(seg->Append(b.data(), b.size()));
    CHECK_EQ(seg->size(), uint64_t{a.size() + b.size()});

    std::vector<uint8_t> got;
    CHECK(seg->ReadAt(0, a.size(), &got));
    CHECK(got == a);
    CHECK(seg->ReadAt(a.size(), b.size(), &got));
    CHECK(got == b);

    // The bytes on disk are exactly the two batches, back to back.
    std::vector<uint8_t> expected = a;
    expected.insert(expected.end(), b.begin(), b.end());
    CHECK(ReadFileBytes(seg->path()) == expected);
}

void test_size_survives_reopen() {
    TempDir dir;
    const Layout layout = write_three_batches(dir.path());

    auto seg = Segment::OpenExisting(dir.path(), 0);
    CHECK_EQ(seg->size(), layout.total);

    // Appending after reopening continues at the end.
    const auto d = batch_at(6, 1);
    CHECK(seg->Append(d.data(), d.size()));
    CHECK_EQ(FileSize(seg->path()), layout.total + d.size());
}

void test_read_out_of_range() {
    TempDir dir;
    write_three_batches(dir.path());
    auto seg = Segment::OpenExisting(dir.path(), 0);

    std::vector<uint8_t> got;
    CHECK(seg->ReadAt(seg->size(), 0, &got));       // empty read at the end
    CHECK(!seg->ReadAt(seg->size(), 1, &got));      // one byte past the end
    CHECK(!seg->ReadAt(seg->size() + 1, 0, &got));  // starts past the end
    CHECK(!seg->ReadAt(1, static_cast<size_t>(-1), &got));  // n would overflow
}

// ---------------------------------------------------------------------------
// Scanning a healthy segment

void test_scan_clean_segment() {
    TempDir dir;
    const Layout layout = write_three_batches(dir.path());
    auto seg = Segment::OpenExisting(dir.path(), 0);

    for (bool verify : {false, true}) {
        std::vector<Seen> seen;
        const ScanResult r = scan_all(*seg, verify, &seen);
        CHECK_EQ(r.stop, ScanStop::kEndOfSegment);
        CHECK(!r.damaged());
        CHECK_EQ(r.valid_end, layout.total);
        CHECK_EQ(r.next_offset, int64_t{6});

        CHECK_EQ(seen.size(), size_t{3});
        if (seen.size() == 3) {
            CHECK_EQ(seen[0].offset, int64_t{0});
            CHECK_EQ(seen[1].offset, int64_t{3});
            CHECK_EQ(seen[2].offset, int64_t{5});
            for (size_t i = 0; i < 3; ++i)
                CHECK_EQ(seen[i].position, layout.starts[i]);
        }
    }
}

void test_scan_empty_segment() {
    TempDir dir;
    auto seg = Segment::Create(dir.path(), 500);
    std::vector<Seen> seen;
    const ScanResult r = scan_all(*seg, true, &seen);
    CHECK_EQ(r.stop, ScanStop::kEndOfSegment);
    CHECK_EQ(r.valid_end, uint64_t{0});
    CHECK_EQ(r.next_offset, int64_t{500});
    CHECK(seen.empty());
}

void test_scan_from_middle() {
    TempDir dir;
    const Layout layout = write_three_batches(dir.path());
    auto seg = Segment::OpenExisting(dir.path(), 0);

    // As a read using an index entry would: start at the second batch.
    std::vector<int64_t> offsets;
    const ScanResult r = seg->Scan(layout.starts[1], 3, false,
                                   [&](const BatchHeader& h, uint64_t) {
                                       offsets.push_back(h.base_offset);
                                       return true;
                                   });
    CHECK_EQ(r.stop, ScanStop::kEndOfSegment);
    CHECK(offsets == std::vector<int64_t>({3, 5}));
}

void test_scan_stopped_by_callback() {
    TempDir dir;
    const Layout layout = write_three_batches(dir.path());
    auto seg = Segment::OpenExisting(dir.path(), 0);

    // Stop when we reach the batch holding offset 4 (the one starting at 3),
    // as PartitionLog::Read does.
    uint64_t found = 0;
    const ScanResult r =
        seg->Scan(0, 0, false, [&](const BatchHeader& h, uint64_t pos) {
            if (h.last_offset() >= 4) {
                found = pos;
                return false;
            }
            return true;
        });
    CHECK_EQ(r.stop, ScanStop::kStoppedByCallback);
    CHECK(!r.damaged());
    CHECK_EQ(found, layout.starts[1]);
    CHECK_EQ(r.valid_end, layout.starts[1]);  // stopped before accepting it
}

// ---------------------------------------------------------------------------
// Scanning a damaged segment, as recovery does

void test_torn_tail_is_incomplete() {
    // Cut the last batch off at every possible point, each time in a fresh
    // directory, and check the scan stops cleanly before it.
    uint64_t last_size = 0;
    {
        TempDir probe;
        const Layout l = write_three_batches(probe.path());
        last_size = l.total - l.starts[2];
    }

    for (uint64_t chop = 1; chop < last_size; ++chop) {
        TempDir dir;
        const Layout layout = write_three_batches(dir.path());
        ChopBytes(dir.file(SegmentFileName(0)), chop);

        auto seg = Segment::OpenExisting(dir.path(), 0);
        std::vector<Seen> seen;
        const ScanResult r = scan_all(*seg, true, &seen);
        CHECK_EQ(r.stop, ScanStop::kIncomplete);
        CHECK(r.damaged());
        CHECK_EQ(r.valid_end, layout.starts[2]);
        CHECK_EQ(r.next_offset, int64_t{5});
        CHECK_EQ(seen.size(), size_t{2});
    }

    // Chopping the whole last batch leaves a clean, shorter segment.
    TempDir dir;
    const Layout layout = write_three_batches(dir.path());
    ChopBytes(dir.file(SegmentFileName(0)), last_size);
    auto seg = Segment::OpenExisting(dir.path(), 0);
    std::vector<Seen> seen;
    const ScanResult r = scan_all(*seg, true, &seen);
    CHECK_EQ(r.stop, ScanStop::kEndOfSegment);
    CHECK_EQ(r.valid_end, layout.starts[2]);
}

void test_flipped_byte_needs_crc_to_detect() {
    TempDir dir;
    const Layout layout = write_three_batches(dir.path());
    // A byte inside the last batch's records (past its 24-byte header).
    FlipByte(dir.file(SegmentFileName(0)), layout.starts[2] + 30);

    auto seg = Segment::OpenExisting(dir.path(), 0);

    std::vector<Seen> with_crc;
    const ScanResult checked = scan_all(*seg, true, &with_crc);
    CHECK_EQ(checked.stop, ScanStop::kCorrupt);
    CHECK_EQ(checked.valid_end, layout.starts[2]);
    CHECK_EQ(with_crc.size(), size_t{2});

    // Without the CRC, the header still looks fine, so nothing is noticed.
    // That's the trade verify_crc makes.
    std::vector<Seen> without_crc;
    const ScanResult unchecked = scan_all(*seg, false, &without_crc);
    CHECK_EQ(unchecked.stop, ScanStop::kEndOfSegment);
    CHECK_EQ(without_crc.size(), size_t{3});
}

void test_offset_gap_is_detected() {
    TempDir dir;
    auto seg = Segment::Create(dir.path(), 0);
    const auto a = batch_at(0, 3);
    const auto wrong = batch_at(10, 2);  // should have been 3
    CHECK(seg->Append(a.data(), a.size()));
    CHECK(seg->Append(wrong.data(), wrong.size()));

    std::vector<Seen> seen;
    const ScanResult r = scan_all(*seg, true, &seen);
    CHECK_EQ(r.stop, ScanStop::kOffsetMismatch);
    CHECK(r.damaged());
    CHECK_EQ(r.valid_end, uint64_t{a.size()});
    CHECK_EQ(r.next_offset, int64_t{3});
    CHECK_EQ(seen.size(), size_t{1});
}

void test_first_batch_must_match_file_name() {
    // A segment named for offset 500 whose first batch says 0.
    TempDir dir;
    auto seg = Segment::Create(dir.path(), 500);
    const auto b = batch_at(0, 1);
    CHECK(seg->Append(b.data(), b.size()));

    std::vector<Seen> seen;
    const ScanResult r = scan_all(*seg, false, &seen);
    CHECK_EQ(r.stop, ScanStop::kOffsetMismatch);
    CHECK_EQ(r.valid_end, uint64_t{0});
}

void test_garbage_header_is_corrupt() {
    TempDir dir;
    const Layout layout = write_three_batches(dir.path());
    // Overwrite the second batch's batch_length (bytes 8-11) with 0xFFFFFFFF,
    // which reads as -1: an impossible length.
    std::vector<uint8_t> bytes = ReadFileBytes(dir.file(SegmentFileName(0)));
    for (uint64_t i = 8; i < 12; ++i) bytes[layout.starts[1] + i] = 0xFF;
    WriteFileBytes(dir.file(SegmentFileName(0)), bytes);

    auto seg = Segment::OpenExisting(dir.path(), 0);
    std::vector<Seen> seen;
    const ScanResult r = scan_all(*seg, false, &seen);
    CHECK_EQ(r.stop, ScanStop::kCorrupt);
    CHECK_EQ(r.valid_end, layout.starts[1]);
    CHECK_EQ(seen.size(), size_t{1});
}

// ---------------------------------------------------------------------------
// Truncating, as recovery and followers do

void test_truncate() {
    TempDir dir;
    const Layout layout = write_three_batches(dir.path());

    {
        auto seg = Segment::OpenExisting(dir.path(), 0);
        CHECK(!seg->Truncate(layout.total + 1));  // can't grow a file this way
        CHECK(seg->Truncate(layout.starts[1]));
        CHECK(seg->Sync());
        CHECK_EQ(seg->size(), layout.starts[1]);
    }

    auto seg = Segment::OpenExisting(dir.path(), 0);
    CHECK_EQ(seg->size(), layout.starts[1]);
    std::vector<Seen> seen;
    const ScanResult r = scan_all(*seg, true, &seen);
    CHECK_EQ(r.stop, ScanStop::kEndOfSegment);
    CHECK_EQ(r.next_offset, int64_t{3});
    CHECK_EQ(seen.size(), size_t{1});

    // New batches continue cleanly after the cut.
    const auto replacement = batch_at(3, 4);
    CHECK(seg->Append(replacement.data(), replacement.size()));
    seen.clear();
    CHECK_EQ(scan_all(*seg, true, &seen).next_offset, int64_t{7});
}

void test_recover_by_truncating_at_valid_end() {
    // The whole recovery step in miniature: scan, truncate at valid_end,
    // and the segment is clean again.
    TempDir dir;
    const Layout layout = write_three_batches(dir.path());
    ChopBytes(dir.file(SegmentFileName(0)), 7);

    auto seg = Segment::OpenExisting(dir.path(), 0);
    std::vector<Seen> seen;
    const ScanResult r = scan_all(*seg, true, &seen);
    CHECK(r.damaged());
    CHECK(seg->Truncate(r.valid_end));
    CHECK(seg->Sync());

    seen.clear();
    const ScanResult after = scan_all(*seg, true, &seen);
    CHECK_EQ(after.stop, ScanStop::kEndOfSegment);
    CHECK_EQ(after.valid_end, layout.starts[2]);
    CHECK_EQ(after.next_offset, int64_t{5});
}

}  // namespace
}  // namespace tributary

int main() {
    tributary::test_file_names();
    tributary::test_create_and_open();
    tributary::test_append_and_read_back();
    tributary::test_size_survives_reopen();
    tributary::test_read_out_of_range();
    tributary::test_scan_clean_segment();
    tributary::test_scan_empty_segment();
    tributary::test_scan_from_middle();
    tributary::test_scan_stopped_by_callback();
    tributary::test_torn_tail_is_incomplete();
    tributary::test_flipped_byte_needs_crc_to_detect();
    tributary::test_offset_gap_is_detected();
    tributary::test_first_batch_must_match_file_name();
    tributary::test_garbage_header_is_corrupt();
    tributary::test_truncate();
    tributary::test_recover_by_truncating_at_valid_end();
    return tributary::ReportResults();
}
