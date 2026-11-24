#include "partition_state.h"

#include <filesystem>
#include <vector>

#include "common/bytes.h"
#include "common/crc32c.h"
#include "storage/file_io.h"

namespace tributary {
namespace {

constexpr uint8_t kStateVersion = 1;
constexpr size_t kStateBodySize = 1 + 4 + 8;  // version + epoch + watermark
constexpr size_t kStateFileSize = kStateBodySize + 4;  // + crc checksum

/**
 * Get the state file's filepath for a given directory.
 */
std::string StatePath(const std::string& dir) {
    return (std::filesystem::path(dir) / kPartitionStateFile).string();
}

}  // namespace

bool SavePartitionState(const std::string& dir, const PartitionState& state) {
    ByteWriter writer(kStateFileSize);
    writer.u8(kStateVersion);
    writer.i32(state.leader_epoch);
    writer.i64(state.high_watermark);
    writer.u32(crc32c(writer.data(), writer.size()));
    return WriteFileAtomic(StatePath(dir), writer.take());
}

StateStatus LoadPartitionState(const std::string& dir, PartitionState* state) {
    std::vector<uint8_t> bytes;
    if (!ReadWholeFile(StatePath(dir), &bytes)) {
        return errno == ENOENT ? StateStatus::kMissing : StateStatus::kIoError;
    }
    if (bytes.size() != kStateFileSize) return StateStatus::kCorrupt;

    ByteReader reader(bytes);
    const uint8_t version = reader.u8();
    const int32_t epoch = reader.i32();
    const int64_t hw = reader.i64();
    const uint32_t stored_crc = reader.u32();

    if (!reader.ok()) return StateStatus::kCorrupt;

    if (crc32c(bytes.data(), kStateBodySize) != stored_crc)
        return StateStatus::kCorrupt;
    if (version != kStateVersion) return StateStatus::kCorrupt;
    if (epoch < 0 || hw < 0) return StateStatus::kCorrupt;

    state->leader_epoch = epoch;
    state->high_watermark = hw;
    return StateStatus::kOk;
}

}  // namespace tributary
