/**
 * Request and response message encoding.
 *
 * Messages travel as a frame (net/frame.h) with payload:
 *
 *      Bytes           Field               Type
 *      0-1             type                i16
 *      2-5             correlation_id      i32
 *      6-...           body
 */
#pragma once

#include <cstdint>
#include <vector>

#include "common/bytes.h"
#include "protocol/error_code.h"

namespace tributary {

/**
 * Type field for messages. Numbers are part of the wire format, with responses
 * simply adding 1 to the request type. Requests are odd, responses are the next
 * even number.
 */
enum class MessageType : int16_t {
    kProduceRequest = 1,
    kProduceResponse = 2,
    kFetchRequest = 3,
    kFetchResponse = 4,
    kMetadataRequest = 5,
    kMetadataResponse = 6,
};

/**
 * Message type to string for logs (e.g. "FetchRequest").
 *
 * Unfamiliar codes give "Unknown".
 */
const char* MessageTypeName(MessageType type);

/**
 * Start of every message.
 */
struct MessageHeader {
    MessageType type;
    int32_t correlation_id;
};

constexpr size_t kMessageHeaderSize = 6;
// acks value for "wait until all replicas have this batch"
constexpr int16_t kAcksAll = -1;
// replica_id used by consumers (followers use their broker id)
constexpr int32_t kConsumerReplicaId = -1;

// -------------- PRODUCER MESSAGING -------------- //

/**
 * One batch for one partition.
 */
struct ProduceEntry {
    std::string topic;
    int32_t partition = 0;
    std::vector<uint8_t> batch;  // one single encoded batch
};

struct ProduceRequest {
    static constexpr MessageType kType = MessageType::kProduceRequest;
    int16_t acks = kAcksAll;
    int32_t timeout_ms = 0;
    std::vector<ProduceEntry> entries;
};

struct ProduceResult {
    std::string topic;
    int32_t partition = 0;
    ErrorCode error = ErrorCode::kNone;
    int64_t base_offset = -1;
};

struct ProduceResponse {
    static constexpr MessageType kType = MessageType::kProduceResponse;
    std::vector<ProduceResult> results;
};

// -------------- PRODUCER MESSAGING -------------- //

struct FetchEntry {
    std::string topic;
    int32_t partition = 0;
    int64_t fetch_offset = 0;
    int32_t leader_epoch = -1;
    int32_t max_bytes = 0;
};

struct FetchRequest {
    static constexpr MessageType kType = MessageType::kFetchRequest;
    int32_t replica_id = kConsumerReplicaId;
    int32_t max_wait_ms = 0;
    int32_t min_bytes = 0;
    std::vector<FetchEntry> entries;
};

struct FetchResult {
    std::string topic;
    int32_t partition = 0;
    ErrorCode error = ErrorCode::kNone;
    int64_t high_watermark = 0;
    int64_t log_start_offset = 0;
    std::vector<uint8_t> batches;
};

struct FetchResponse {
    static constexpr MessageType kType = MessageType::kFetchResponse;
    std::vector<FetchResult> results;
};

// -------------- METADATA -------------- //

struct MetadataRequest {
    static constexpr MessageType kType = MessageType::kMetadataRequest;
    std::vector<std::string> topics;  // Empty means every topic
};

struct BrokerInfo {
    int32_t broker_id = 0;
    std::string host;
    uint16_t port = 0;
};

struct PartitionMetadata {
    int32_t partition = 0;
    int32_t leader_id = -1;
    int32_t leader_epoch = 0;
};

struct TopicMetadata {
    std::string name;
    ErrorCode error = ErrorCode::kNone;
    std::vector<PartitionMetadata> partitions;
};

struct MetadataResponse {
    static constexpr MessageType kType = MessageType::kMetadataResponse;
    std::vector<BrokerInfo> brokers;
    std::vector<TopicMetadata> topics;
};

// -------------- BODY ENCODING/DECODING -------------- //

void Encode(ByteWriter& writer, const ProduceRequest& msg);
void Encode(ByteWriter& writer, const ProduceResponse& msg);
void Encode(ByteWriter& writer, const FetchRequest& msg);
void Encode(ByteWriter& writer, const FetchResponse& msg);
void Encode(ByteWriter& writer, const MetadataRequest& msg);
void Encode(ByteWriter& writer, const MetadataResponse& msg);

bool Decode(ByteReader& reader, ProduceRequest* msg);
bool Decode(ByteReader& reader, ProduceResponse* msg);
bool Decode(ByteReader& reader, FetchRequest* msg);
bool Decode(ByteReader& reader, FetchResponse* msg);
bool Decode(ByteReader& reader, MetadataRequest* msg);
bool Decode(ByteReader& reader, MetadataResponse* msg);

// -------------- WHOLE MESSAGE ENCODING/DECODING -------------- //

/**
 * Reads the header of a frame into *header to determine which message type to
 * decode. Returns false if the payload is shorter than the header.
 */
bool PeekHeader(const std::vector<uint8_t>& payload, MessageHeader* header);

/**
 * Encodes msg with a header carrying Msg::kType and correlation_id, ready to
 * send as one frame.
 */
template <typename Msg>
std::vector<uint8_t> EncodeMessage(int32_t correlation_id, const Msg& msg) {
    ByteWriter writer;
    writer.i16(static_cast<int16_t>(Msg::kType));
    writer.i32(correlation_id);
    Encode(writer, msg);
    return writer.take();
}

/**
 * Decodes a whole frame payload as a Msg.
 *
 * Fails if the header's type isn't Msg::kType, if fields are malformed, or if
 * bytes are still left over.
 *
 * Stores correlation id in *correlation_id if successful.
 */
template <typename Msg>
bool DecodeMessage(const std::vector<uint8_t>& payload, Msg* msg,
                   int32_t* correlation_id = nullptr) {
    ByteReader reader(payload);
    const auto type = static_cast<MessageType>(reader.i16());
    const int32_t cid = reader.i32();
    if (!reader.ok() || type != Msg::kType) return false;
    if (!Decode(reader, msg) || !reader.ok() || reader.remaining() != 0) {
        return false;
    }
    if (correlation_id) {
        *correlation_id = cid;
    }
    return true;
}

}  // namespace tributary
