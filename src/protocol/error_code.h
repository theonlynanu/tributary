/**
 * Error codes carried in responses. Each is an i16 on the wire.
 */

#pragma once

#include <cstdint>

namespace tributary {

enum class ErrorCode : int16_t {
    kNone = 0,                     // success
    kUnknownServerError = 1,       // bug or I/O error on server
    kInvalidRequest = 2,           // malformed or bad request
    kCorruptMessage = 3,           // batch failed checksum or header checks
    kUnknownTopicOrPartition = 4,  // no such partition or topic
    kNotLeader = 5,                // broker doesn't lead partition
    kFencedEpoch = 6,              // request's leader_epoch is out of date
    kOffsetOutOfRange = 7,         // fetch offset is outside of log
    kNotEnoughReplicas = 8,        // in-sync set is too small
    kRequestTimedOut = 9,          // acks=all not satisfied in time
    kRebalanceInProgress = 10,     // consumer group is reassigning partitions
    kIllegalGeneration = 11,       // consumer group generation is out of date
    kUnknownMember = 12,           // coordinator unaware of this consumer
    kUnknownBroker = 13,           // controller unaware of this broker

};

/**
 * Code to string for logs (e.g. "NOT_LEADER").
 *
 * Unfamiliar codes give "UNKNOWN_ERROR_CODE".
 */
const char* ErrorName(ErrorCode code);

}  // namespace tributary
