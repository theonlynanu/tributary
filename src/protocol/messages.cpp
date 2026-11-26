#include "messages.h"

#include <utility>

#include "common/bytes.h"

namespace tributary {
namespace {

// --------------- PRIMITIVES --------------- //

// Smallest possible encoded size of one list element, per type. Used to reject
// a list count that can't fit int he bytes left before reserving memory for it
// (corrupt count of 2 billion shouldn't be allocated).

// topic, partition, batch
constexpr size_t kMinProduceEntry = 2 + 4 + 4;
// topic, partition, error, offset
constexpr size_t kMinProduceResult = 2 + 4 + 2 + 8;
// topic .. max_bytes
constexpr size_t kMinFetchEntry = 2 + 4 + 8 + 4 + 4;
// topic .. batches
constexpr size_t kMinFetchResult = 2 + 4 + 2 + 8 + 8 + 4;
constexpr size_t kMinString = 2;
// id, host, port
constexpr size_t kMinBrokerInfo = 4 + 2 + 2;
constexpr size_t kMinPartitionMetadata = 4 + 4 + 4;
// name, error, partitions
constexpr size_t kMinTopicMetadata = 2 + 2 + 4;

void PutBytes(ByteWriter& writer, const std::vector<uint8_t>& v) {
    writer.blob(v.data(), v.size());
}

std::vector<uint8_t> GetBytes(ByteReader& reader) {
    const ByteView view = reader.blob();
    return std::vector<uint8_t>(view.data, view.data + view.size);
}

void PutError(ByteWriter& writer, ErrorCode e) {
    writer.i16(static_cast<int16_t>(e));
}

ErrorCode GetError(ByteReader& reader) {
    return static_cast<ErrorCode>(reader.i16());
}

// list<T> is an i32 count, then each element is written by put
template <typename T, typename PutFn>
void PutList(ByteWriter& writer, const std::vector<T>& items, PutFn put) {
    writer.i32(static_cast<int32_t>(items.size()));
    for (const T& item : items) {
        put(writer, item);
    }
}

/**
 * Reads a list<T>, replacing *out. get reads one element and returns false if
 * malformed.
 */
template <typename T, typename GetFn>
bool GetList(ByteReader& reader, size_t min_item_size, std::vector<T>* out,
             GetFn get) {
    const int32_t count = reader.i32();

    if (!reader.ok() || count < 0) return false;
    if (static_cast<size_t>(count) > reader.remaining() / min_item_size)
        return false;

    std::vector<T> items;
    items.reserve(static_cast<size_t>(count));

    for (int32_t i = 0; i < count; i++) {
        T item;
        if (!get(reader, &item) || !reader.ok()) return false;
        items.push_back(std::move(item));
    }
    *out = std::move(items);
    return true;
}

// --------------- ELEMENT ENCODERS --------------- //

void Put(ByteWriter& w, const ProduceEntry& e) {
    w.str(e.topic);
    w.i32(e.partition);
    PutBytes(w, e.batch);
}
bool Get(ByteReader& r, ProduceEntry* e) {
    e->topic = r.str();
    e->partition = r.i32();
    e->batch = GetBytes(r);
    return r.ok();
}

void Put(ByteWriter& w, const ProduceResult& e) {
    w.str(e.topic);
    w.i32(e.partition);
    PutError(w, e.error);
    w.i64(e.base_offset);
}
bool Get(ByteReader& r, ProduceResult* e) {
    e->topic = r.str();
    e->partition = r.i32();
    e->error = GetError(r);
    e->base_offset = r.i64();
    return r.ok();
}

void Put(ByteWriter& w, const FetchEntry& e) {
    w.str(e.topic);
    w.i32(e.partition);
    w.i64(e.fetch_offset);
    w.i32(e.leader_epoch);
    w.i32(e.max_bytes);
}
bool Get(ByteReader& r, FetchEntry* e) {
    e->topic = r.str();
    e->partition = r.i32();
    e->fetch_offset = r.i64();
    e->leader_epoch = r.i32();
    e->max_bytes = r.i32();
    return r.ok();
}

void Put(ByteWriter& w, const FetchResult& e) {
    w.str(e.topic);
    w.i32(e.partition);
    PutError(w, e.error);
    w.i64(e.high_watermark);
    w.i64(e.log_start_offset);
    PutBytes(w, e.batches);
}
bool Get(ByteReader& r, FetchResult* e) {
    e->topic = r.str();
    e->partition = r.i32();
    e->error = GetError(r);
    e->high_watermark = r.i64();
    e->log_start_offset = r.i64();
    e->batches = GetBytes(r);
    return r.ok();
}

void Put(ByteWriter& w, const BrokerInfo& b) {
    w.i32(b.broker_id);
    w.str(b.host);
    w.u16(b.port);
}
bool Get(ByteReader& r, BrokerInfo* b) {
    b->broker_id = r.i32();
    b->host = r.str();
    b->port = r.u16();
    return r.ok();
}

void Put(ByteWriter& w, const PartitionMetadata& p) {
    w.i32(p.partition);
    w.i32(p.leader_id);
    w.i32(p.leader_epoch);
}
bool Get(ByteReader& r, PartitionMetadata* p) {
    p->partition = r.i32();
    p->leader_id = r.i32();
    p->leader_epoch = r.i32();
    return r.ok();
}

void Put(ByteWriter& w, const TopicMetadata& t) {
    w.str(t.name);
    PutError(w, t.error);
    PutList(w, t.partitions,
            [](ByteWriter& w2, const PartitionMetadata& p) { Put(w2, p); });
}
bool Get(ByteReader& r, TopicMetadata* t) {
    t->name = r.str();
    t->error = GetError(r);
    return GetList(
        r, kMinPartitionMetadata, &t->partitions,
        [](ByteReader& r2, PartitionMetadata* p) { return Get(r2, p); });
}

// Lambdas that forward to the Put/Get overloads for PutList/GetList
constexpr auto kPut = [](ByteWriter& w, const auto& item) { Put(w, item); };
constexpr auto kGet = [](ByteReader& r, auto* item) { return Get(r, item); };

}  // namespace

const char* MessageTypeName(MessageType type) {
    switch (type) {
        case MessageType::kProduceRequest:
            return "ProduceRequest";
        case MessageType::kProduceResponse:
            return "ProduceResponse";
        case MessageType::kFetchRequest:
            return "FetchRequest";
        case MessageType::kFetchResponse:
            return "FetchResponse";
        case MessageType::kMetadataRequest:
            return "MetadataRequest";
        case MessageType::kMetadataResponse:
            return "MetaDdtaResponse";
    }

    return "Unknown";
}

// --------------- MESSAGE BODIES --------------- //

void Encode(ByteWriter& w, const ProduceRequest& m) {
    w.i16(m.acks);
    w.i32(m.timeout_ms);
    PutList(w, m.entries, kPut);
}
bool Decode(ByteReader& r, ProduceRequest* m) {
    m->acks = r.i16();
    m->timeout_ms = r.i32();
    return r.ok() && GetList(r, kMinProduceEntry, &m->entries, kGet);
}

void Encode(ByteWriter& w, const ProduceResponse& m) {
    PutList(w, m.results, kPut);
}
bool Decode(ByteReader& r, ProduceResponse* m) {
    return GetList(r, kMinProduceResult, &m->results, kGet);
}

void Encode(ByteWriter& w, const FetchRequest& m) {
    w.i32(m.replica_id);
    w.i32(m.max_wait_ms);
    w.i32(m.min_bytes);
    PutList(w, m.entries, kPut);
}
bool Decode(ByteReader& r, FetchRequest* m) {
    m->replica_id = r.i32();
    m->max_wait_ms = r.i32();
    m->min_bytes = r.i32();
    return r.ok() && GetList(r, kMinFetchEntry, &m->entries, kGet);
}

void Encode(ByteWriter& w, const FetchResponse& m) {
    PutList(w, m.results, kPut);
}
bool Decode(ByteReader& r, FetchResponse* m) {
    return GetList(r, kMinFetchResult, &m->results, kGet);
}

void Encode(ByteWriter& w, const MetadataRequest& m) {
    PutList(w, m.topics,
            [](ByteWriter& w2, const std::string& s) { w2.str(s); });
}
bool Decode(ByteReader& r, MetadataRequest* m) {
    return GetList(r, kMinString, &m->topics,
                   [](ByteReader& r2, std::string* s) {
                       *s = r2.str();
                       return r2.ok();
                   });
}

void Encode(ByteWriter& w, const MetadataResponse& m) {
    PutList(w, m.brokers, kPut);
    PutList(w, m.topics, kPut);
}
bool Decode(ByteReader& r, MetadataResponse* m) {
    return GetList(r, kMinBrokerInfo, &m->brokers, kGet) &&
           GetList(r, kMinTopicMetadata, &m->topics, kGet);
}

// --------------- HEADERS --------------- //
bool PeekHeader(const std::vector<uint8_t>& payload, MessageHeader* header) {
    ByteReader r(payload);
    const int16_t type = r.i16();
    const int32_t cid = r.i32();
    if (!r.ok()) return false;
    header->type = static_cast<MessageType>(type);
    header->correlation_id = cid;
    return true;
}

}  // namespace tributary
