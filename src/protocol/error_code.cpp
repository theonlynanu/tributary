#include "error_code.h"

namespace tributary {

const char* ErrorName(ErrorCode code) {
    switch (code) {
        case ErrorCode::kNone:
            return "NONE";
        case ErrorCode::kUnknownServerError:
            return "UNKNOWN_SERVER_ERROR";
        case ErrorCode::kInvalidRequest:
            return "INVALID_REQUEST";
        case ErrorCode::kCorruptMessage:
            return "CORRUPT_MESSAGE";
        case ErrorCode::kUnknownTopicOrPartition:
            return "UNKNOWN_TOPIC_OR_PARTITION";
        case ErrorCode::kNotLeader:
            return "NOT_LEADER";
        case ErrorCode::kFencedEpoch:
            return "FENCED_EPOCH";
        case ErrorCode::kOffsetOutOfRange:
            return "OFFSET_OUT_OF_RANGE";
        case ErrorCode::kNotEnoughReplicas:
            return "NOT_ENOUGH_REPLICAS";
        case ErrorCode::kRequestTimedOut:
            return "REQUEST_TIMED_OUT";
        case ErrorCode::kRebalanceInProgress:
            return "REBALANCE_IN_PROGRESS";
        case ErrorCode::kIllegalGeneration:
            return "ILLEGAL_GENERATION";
        case ErrorCode::kUnknownMember:
            return "UNKNOWN_MEMBER";
        case ErrorCode::kUnknownBroker:
            return "UNKNOWN_BROKER";
    }
    return "UNKNOWN_ERROR_CODE";
}

}  // namespace tributary
