/**
 * The part of replica state that must survive a restart.
 *
 * Each partition directory has one state file alongside its segments. It
 * records the last leader epoch the replica knew of and the last known high
 * watermark. Restarted followers truncate logs to the high watermark before
 * fetching, and the leader epoch allows it to reject stale messages from
 * previous leaders.
 *
 * File format (network order):
 *
 *    Bytes         Field          Type
 *      0          Version          u8
 *     1-4       leader_epoch       i32
 *     5-12     high_watermark      i64
 *    13-16          crc            u32
 */
#pragma once
#include <cstdint>
#include <string>

namespace tributary {

/**
 * Name of the state file.
 */
constexpr char kPartitionStateFile[] = "partition.state";

struct PartitionState {
    int32_t leader_epoch = 0;
    int64_t high_watermark = 0;
};

/**
 * Result of trying to load a state file
 */
enum class StateStatus {
    kOk,       // loaded
    kMissing,  // no state file found
    kCorrupt,  // file exists but is corrupted
    kIoError   // file could not be read
};

/**
 * Writes the PartitionState to the state file in dir, replacing existing ones
 * atomically and durably.
 *
 * Returns false on failure with errno set. Old file unchanged on failure.
 */
bool SavePartitionState(const std::string& dir, const PartitionState& state);

/**
 * Reads the state file in dir into *state. *state is only written when result
 * is kOk.
 */
StateStatus LoadPartitionState(const std::string& dir, PartitionState* state);

}  // namespace tributary
