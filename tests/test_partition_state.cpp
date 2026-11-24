#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "common/bytes.h"
#include "common/crc32c.h"
#include "storage/partition_state.h"
#include "test.h"
#include "test_util.h"

namespace tributary {

std::ostream& operator<<(std::ostream& os, StateStatus s) {
    switch (s) {
        case StateStatus::kOk:
            return os << "kOk";
        case StateStatus::kMissing:
            return os << "kMissing";
        case StateStatus::kCorrupt:
            return os << "kCorrupt";
        case StateStatus::kIoError:
            return os << "kIoError";
    }
    return os << "StateStatus(" << static_cast<int>(s) << ")";
}

namespace {

// Builds a state file by hand, with a correct CRC, so tests can plant
// values the real writer would never produce.
std::vector<uint8_t> raw_state(uint8_t version, int32_t epoch, int64_t hw) {
    ByteWriter w;
    w.u8(version);
    w.i32(epoch);
    w.i64(hw);
    w.u32(crc32c(w.data(), w.size()));
    return w.take();
}

void test_missing_file() {
    TempDir dir;
    PartitionState s{5, 9};
    CHECK_EQ(LoadPartitionState(dir.path(), &s), StateStatus::kMissing);
    CHECK_EQ(s.leader_epoch, 5);  // untouched
    CHECK_EQ(s.high_watermark, int64_t{9});
}

void test_round_trip_and_overwrite() {
    TempDir dir;
    CHECK(SavePartitionState(dir.path(), PartitionState{3, 1200}));

    PartitionState s;
    CHECK_EQ(LoadPartitionState(dir.path(), &s), StateStatus::kOk);
    CHECK_EQ(s.leader_epoch, 3);
    CHECK_EQ(s.high_watermark, int64_t{1200});

    CHECK(SavePartitionState(dir.path(), PartitionState{4, 1500}));
    CHECK_EQ(LoadPartitionState(dir.path(), &s), StateStatus::kOk);
    CHECK_EQ(s.leader_epoch, 4);
    CHECK_EQ(s.high_watermark, int64_t{1500});

    CHECK(ListFiles(dir.path()) == std::vector<std::string>{"partition.state"});
}

void test_exact_layout() {
    TempDir dir;
    CHECK(SavePartitionState(dir.path(), PartitionState{3, 1200}));
    const std::vector<uint8_t> bytes =
        ReadFileBytes(dir.file(kPartitionStateFile));
    CHECK(bytes == raw_state(1, 3, 1200));
    CHECK_EQ(bytes.size(), size_t{17});
}

void test_any_flipped_byte_is_corrupt() {
    TempDir dir;
    CHECK(SavePartitionState(dir.path(), PartitionState{3, 1200}));
    const std::vector<uint8_t> good =
        ReadFileBytes(dir.file(kPartitionStateFile));

    for (size_t i = 0; i < good.size(); ++i) {
        std::vector<uint8_t> bad = good;
        bad[i] ^= 0x01;
        WriteFileBytes(dir.file(kPartitionStateFile), bad);
        PartitionState s{77, 77};
        CHECK_EQ(LoadPartitionState(dir.path(), &s), StateStatus::kCorrupt);
        CHECK_EQ(s.leader_epoch, 77);
    }
}

void test_wrong_size_is_corrupt() {
    TempDir dir;
    PartitionState s;
    std::vector<uint8_t> bytes = raw_state(1, 3, 1200);

    bytes.pop_back();
    WriteFileBytes(dir.file(kPartitionStateFile), bytes);
    CHECK_EQ(LoadPartitionState(dir.path(), &s), StateStatus::kCorrupt);

    WriteFileBytes(dir.file(kPartitionStateFile), {});
    CHECK_EQ(LoadPartitionState(dir.path(), &s), StateStatus::kCorrupt);

    bytes = raw_state(1, 3, 1200);
    bytes.push_back(0);
    WriteFileBytes(dir.file(kPartitionStateFile), bytes);
    CHECK_EQ(LoadPartitionState(dir.path(), &s), StateStatus::kCorrupt);
}

void test_rejects_impossible_values() {
    TempDir dir;
    PartitionState s;

    WriteFileBytes(dir.file(kPartitionStateFile),
                   raw_state(2, 3, 1200));  // future version
    CHECK_EQ(LoadPartitionState(dir.path(), &s), StateStatus::kCorrupt);

    WriteFileBytes(dir.file(kPartitionStateFile), raw_state(1, -1, 1200));
    CHECK_EQ(LoadPartitionState(dir.path(), &s), StateStatus::kCorrupt);

    WriteFileBytes(dir.file(kPartitionStateFile), raw_state(1, 3, -5));
    CHECK_EQ(LoadPartitionState(dir.path(), &s), StateStatus::kCorrupt);
}

void test_save_into_missing_directory_fails() {
    TempDir dir;
    CHECK(!SavePartitionState(dir.file("no-such-dir"), PartitionState{1, 1}));
}

}  // namespace
}  // namespace tributary

int main() {
    tributary::test_missing_file();
    tributary::test_round_trip_and_overwrite();
    tributary::test_exact_layout();
    tributary::test_any_flipped_byte_is_corrupt();
    tributary::test_wrong_size_is_corrupt();
    tributary::test_rejects_impossible_values();
    tributary::test_save_into_missing_directory_fails();
    return tributary::ReportResults();
}
