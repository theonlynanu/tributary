#include <cstdint>

#include "storage/offset_index.h"
#include "test.h"

namespace tributary {
namespace {

// Compares a lookup result against an expected {offset, position}.
void check_entry(const OffsetIndex::Entry& e, int64_t offset,
                 uint64_t position) {
    CHECK_EQ(e.offset, offset);
    CHECK_EQ(e.position, position);
}

// Batches at these (offset, position) pairs, with a 100-byte interval,
// should keep entries at positions 0, 120 and 260 only:
//   0 → kept (first), 40 → 40 < 100, 120 → 120 ≥ 100,
//   160 → 40 < 100, 260 → 140 ≥ 100.
OffsetIndex sample_index() {
    OffsetIndex idx(0, 100);
    idx.MaybeAdd(0, 0);
    idx.MaybeAdd(3, 40);
    idx.MaybeAdd(7, 120);
    idx.MaybeAdd(9, 160);
    idx.MaybeAdd(14, 260);
    return idx;
}

void test_sparse_entries() {
    const OffsetIndex idx = sample_index();
    CHECK_EQ(idx.size(), size_t{3});
}

void test_lookup() {
    const OffsetIndex idx = sample_index();
    check_entry(idx.Lookup(0), 0, 0);        // exactly the first entry
    check_entry(idx.Lookup(5), 0, 0);        // between entries
    check_entry(idx.Lookup(7), 7, 120);      // exactly an entry
    check_entry(idx.Lookup(13), 7, 120);     // just before the next entry
    check_entry(idx.Lookup(14), 14, 260);    // exactly the last entry
    check_entry(idx.Lookup(1000), 14, 260);  // past the end
}

void test_lookup_before_first_entry() {
    // A segment starting at offset 500: anything earlier maps to its start.
    OffsetIndex idx(500, 100);
    idx.MaybeAdd(500, 0);
    idx.MaybeAdd(520, 300);
    check_entry(idx.Lookup(10), 500, 0);
    check_entry(idx.Lookup(519), 500, 0);
    check_entry(idx.Lookup(520), 520, 300);
}

void test_empty_index() {
    const OffsetIndex idx(500, 100);
    CHECK(idx.empty());
    check_entry(idx.Lookup(0), 500, 0);
    check_entry(idx.Lookup(999), 500, 0);
}

void test_truncate() {
    OffsetIndex idx = sample_index();

    // Cutting at an entry's position removes that entry and later ones.
    idx.TruncateTo(120);
    CHECK_EQ(idx.size(), size_t{1});
    check_entry(idx.Lookup(13), 0, 0);

    // Cutting between entries keeps the earlier ones.
    OffsetIndex idx2 = sample_index();
    idx2.TruncateTo(200);
    CHECK_EQ(idx2.size(), size_t{2});
    check_entry(idx2.Lookup(14), 7, 120);

    // Cutting past the end changes nothing.
    OffsetIndex idx3 = sample_index();
    idx3.TruncateTo(10000);
    CHECK_EQ(idx3.size(), size_t{3});
}

void test_truncate_to_zero_then_reuse() {
    OffsetIndex idx = sample_index();
    idx.TruncateTo(0);
    CHECK(idx.empty());

    // After emptying, the next batch is treated as the first again.
    idx.MaybeAdd(0, 0);
    CHECK_EQ(idx.size(), size_t{1});

    // And the interval is measured from the new last entry.
    idx.MaybeAdd(4, 50);
    CHECK_EQ(idx.size(), size_t{1});
    idx.MaybeAdd(8, 100);
    CHECK_EQ(idx.size(), size_t{2});
}

void test_truncate_then_append_continues() {
    // After a follower truncates mid-segment, appends resume from the new
    // end and the interval is measured from the last surviving entry.
    OffsetIndex idx = sample_index();  // entries at 0, 120, 260
    idx.TruncateTo(200);               // entries at 0, 120
    idx.MaybeAdd(10, 200);             // 80 since 120: not kept
    CHECK_EQ(idx.size(), size_t{2});
    idx.MaybeAdd(12, 230);  // 110 since 120: kept
    CHECK_EQ(idx.size(), size_t{3});
    check_entry(idx.Lookup(12), 12, 230);
}

void test_interval_of_one_indexes_everything() {
    OffsetIndex idx(0, 1);
    for (int64_t i = 0; i < 50; ++i) {
        idx.MaybeAdd(i * 2, static_cast<uint64_t>(i) * 10);
    }
    CHECK_EQ(idx.size(), size_t{50});
    check_entry(idx.Lookup(31), 30, 150);
}

}  // namespace
}  // namespace tributary

int main() {
    tributary::test_sparse_entries();
    tributary::test_lookup();
    tributary::test_lookup_before_first_entry();
    tributary::test_empty_index();
    tributary::test_truncate();
    tributary::test_truncate_to_zero_then_reuse();
    tributary::test_truncate_then_append_continues();
    tributary::test_interval_of_one_indexes_everything();
    return tributary::ReportResults();
}
