#include <cstdint>
#include <limits>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include "common/bytes.h"
#include "net/connect.h"
#include "net/frame.h"
#include "net/listener.h"
#include "protocol/error_code.h"
#include "protocol/messages.h"
#include "protocol/rpc.h"
#include "test.h"

namespace tributary {

// Printers so CHECK_EQ can show these enums (they must be in namespace
// tributary; see test_util.h).
std::ostream& operator<<(std::ostream& os, ErrorCode e) {
    return os << ErrorName(e);
}
std::ostream& operator<<(std::ostream& os, MessageType t) {
    return os << MessageTypeName(t);
}
std::ostream& operator<<(std::ostream& os, CallStatus s) {
    switch (s) {
        case CallStatus::kOk:
            return os << "kOk";
        case CallStatus::kIoError:
            return os << "kIoError";
        case CallStatus::kBadResponse:
            return os << "kBadResponse";
    }
    return os << "CallStatus(?)";
}

namespace {

std::vector<uint8_t> bytes_of(const std::string& s) {
    return {s.begin(), s.end()};
}

// Sample messages with every field set to something non-default, including
// negative values and empty and non-empty byte fields.
ProduceRequest sample_produce_request() {
    ProduceRequest m;
    m.acks = kAcksAll;
    m.timeout_ms = 5000;
    m.entries = {{"orders", 2, bytes_of("batch-one")}, {"clicks", 0, {}}};
    return m;
}

ProduceResponse sample_produce_response() {
    ProduceResponse m;
    m.results = {{"orders", 2, ErrorCode::kNone, 500},
                 {"clicks", 0, ErrorCode::kNotLeader, -1}};
    return m;
}

FetchRequest sample_fetch_request() {
    FetchRequest m;
    m.replica_id = kConsumerReplicaId;
    m.max_wait_ms = 500;
    m.min_bytes = 1;
    m.entries = {{"orders", 2, 503, 4, 1 << 20}, {"orders", 3, 0, -1, 4096}};
    return m;
}

FetchResponse sample_fetch_response() {
    FetchResponse m;
    m.results = {
        {"orders", 2, ErrorCode::kNone, 510, 0, bytes_of("two-batches")},
        {"orders", 3, ErrorCode::kOffsetOutOfRange, 40, 12, {}}};
    return m;
}

MetadataRequest sample_metadata_request() {
    MetadataRequest m;
    m.topics = {"orders", "clicks"};
    return m;
}

MetadataResponse sample_metadata_response() {
    MetadataResponse m;
    m.brokers = {{1, "linux-075.khoury.northeastern.edu", 41101},
                 {2, "localhost", 41102}};
    m.topics = {
        {"orders", ErrorCode::kNone, {{0, 1, 3}, {1, 2, 3}, {2, -1, 5}}},
        {"missing", ErrorCode::kUnknownTopicOrPartition, {}}};
    return m;
}

// Encode, decode, then encode again: if the two encodings are identical,
// every field survived the round trip. This checks all fields without
// writing an equality operator for every struct. The spot checks in each
// test below make sure the decoded values are also the right ones.
template <typename Msg>
bool round_trips(const Msg& original, Msg* decoded) {
    const std::vector<uint8_t> first = EncodeMessage(77, original);
    int32_t cid = 0;
    if (!DecodeMessage(first, decoded, &cid) || cid != 77) return false;
    return EncodeMessage(77, *decoded) == first;
}

// ---------------------------------------------------------------------------
// Round trips

void test_produce_round_trip() {
    ProduceRequest req;
    CHECK(round_trips(sample_produce_request(), &req));
    CHECK_EQ(req.acks, int16_t{-1});
    CHECK_EQ(req.entries.size(), size_t{2});
    if (req.entries.size() == 2) {
        CHECK_EQ(req.entries[0].topic, std::string("orders"));
        CHECK(req.entries[0].batch == bytes_of("batch-one"));
        CHECK(req.entries[1].batch.empty());
    }

    ProduceResponse resp;
    CHECK(round_trips(sample_produce_response(), &resp));
    if (resp.results.size() == 2) {
        CHECK_EQ(resp.results[0].base_offset, int64_t{500});
        CHECK_EQ(resp.results[1].error, ErrorCode::kNotLeader);
        CHECK_EQ(resp.results[1].base_offset, int64_t{-1});
    }
}

void test_fetch_round_trip() {
    FetchRequest req;
    CHECK(round_trips(sample_fetch_request(), &req));
    CHECK_EQ(req.replica_id, -1);
    if (req.entries.size() == 2) {
        CHECK_EQ(req.entries[0].fetch_offset, int64_t{503});
        CHECK_EQ(req.entries[1].leader_epoch, -1);
    }

    FetchResponse resp;
    CHECK(round_trips(sample_fetch_response(), &resp));
    if (resp.results.size() == 2) {
        CHECK(resp.results[0].batches == bytes_of("two-batches"));
        CHECK_EQ(resp.results[1].error, ErrorCode::kOffsetOutOfRange);
        CHECK_EQ(resp.results[1].log_start_offset, int64_t{12});
    }
}

void test_metadata_round_trip() {
    MetadataRequest req;
    CHECK(round_trips(sample_metadata_request(), &req));
    CHECK(req.topics == std::vector<std::string>({"orders", "clicks"}));

    MetadataResponse resp;
    CHECK(round_trips(sample_metadata_response(), &resp));
    if (resp.brokers.size() == 2) {
        CHECK_EQ(resp.brokers[0].host,
                 std::string("linux-075.khoury.northeastern.edu"));
        CHECK_EQ(resp.brokers[0].port, uint16_t{41101});
    }
    if (resp.topics.size() == 2) {
        CHECK_EQ(resp.topics[0].partitions.size(), size_t{3});
        if (resp.topics[0].partitions.size() == 3) {
            CHECK_EQ(resp.topics[0].partitions[2].leader_id, -1);
        }
        CHECK(resp.topics[1].partitions.empty());
    }
}

void test_empty_messages_round_trip() {
    ProduceRequest a;
    ProduceResponse b;
    FetchRequest c;
    FetchResponse d;
    MetadataRequest e;
    MetadataResponse f;
    CHECK(round_trips(ProduceRequest{}, &a));
    CHECK(round_trips(ProduceResponse{}, &b));
    CHECK(round_trips(FetchRequest{}, &c));
    CHECK(round_trips(FetchResponse{}, &d));
    CHECK(round_trips(MetadataRequest{}, &e));
    CHECK(round_trips(MetadataResponse{}, &f));
}

// ---------------------------------------------------------------------------
// The wire format itself

void test_exact_header_and_body_bytes() {
    MetadataRequest m;
    m.topics = {"ab"};
    const std::vector<uint8_t> bytes = EncodeMessage(0x01020304, m);
    const std::vector<uint8_t> expected = {
        0x00, 0x05,              // type: MetadataRequest (5)
        0x01, 0x02, 0x03, 0x04,  // correlation id
        0x00, 0x00, 0x00, 0x01,  // list count: 1
        0x00, 0x02, 'a',  'b',   // string: length 2, "ab"
    };
    CHECK(bytes == expected);
}

void test_peek_header() {
    const std::vector<uint8_t> bytes =
        EncodeMessage(42, sample_fetch_request());
    MessageHeader h{};
    CHECK(PeekHeader(bytes, &h));
    CHECK_EQ(h.type, MessageType::kFetchRequest);
    CHECK_EQ(h.correlation_id, 42);

    // An unknown type is reported as sent; the server decides what to do.
    std::vector<uint8_t> odd = bytes;
    odd[0] = 0x7F;
    CHECK(PeekHeader(odd, &h));
    CHECK_EQ(static_cast<int>(h.type), 0x7F00 | odd[1]);

    CHECK(!PeekHeader(std::vector<uint8_t>(5, 0), &h));
}

// ---------------------------------------------------------------------------
// Rejecting malformed input

void test_wrong_type_rejected() {
    const std::vector<uint8_t> bytes = EncodeMessage(1, sample_fetch_request());
    FetchResponse wrong;
    CHECK(!DecodeMessage(bytes, &wrong));
}

void test_trailing_bytes_rejected() {
    std::vector<uint8_t> bytes = EncodeMessage(1, sample_produce_request());
    bytes.push_back(0);
    ProduceRequest m;
    CHECK(!DecodeMessage(bytes, &m));
}

// Cut every message off at every possible length: none may decode, and
// none may crash or read past the end (run this under SAN=address).
template <typename Msg>
void check_every_prefix_rejected(const Msg& m) {
    const std::vector<uint8_t> bytes = EncodeMessage(1, m);
    for (size_t len = 0; len < bytes.size(); ++len) {
        const std::vector<uint8_t> prefix(bytes.begin(), bytes.begin() + len);
        Msg out;
        CHECK(!DecodeMessage(prefix, &out));
    }
}

void test_truncated_messages_rejected() {
    check_every_prefix_rejected(sample_produce_request());
    check_every_prefix_rejected(sample_produce_response());
    check_every_prefix_rejected(sample_fetch_request());
    check_every_prefix_rejected(sample_fetch_response());
    check_every_prefix_rejected(sample_metadata_request());
    check_every_prefix_rejected(sample_metadata_response());
}

void test_huge_list_count_rejected_without_allocating() {
    // A ProduceResponse claiming two billion results in a 10-byte payload.
    ByteWriter w;
    w.i16(static_cast<int16_t>(MessageType::kProduceResponse));
    w.i32(1);
    w.i32(std::numeric_limits<int32_t>::max());
    const std::vector<uint8_t> bytes = w.take();

    ProduceResponse m;
    CHECK(!DecodeMessage(bytes, &m));
    CHECK(m.results.capacity() == 0);

    // A negative count is rejected too.
    ByteWriter n;
    n.i16(static_cast<int16_t>(MessageType::kMetadataRequest));
    n.i32(1);
    n.i32(-1);
    MetadataRequest md;
    CHECK(!DecodeMessage(n.take(), &md));
}

void test_unknown_error_code_is_kept() {
    // A newer peer may send a code this build doesn't know. It decodes, and
    // its name says so, rather than the message being rejected.
    ProduceResponse m;
    m.results = {{"t", 0, static_cast<ErrorCode>(999), -1}};
    ProduceResponse out;
    CHECK(round_trips(m, &out));
    if (!out.results.empty()) {
        CHECK_EQ(static_cast<int>(out.results[0].error), 999);
        CHECK_EQ(std::string(ErrorName(out.results[0].error)),
                 std::string("UNKNOWN_ERROR_CODE"));
    }
}

void test_error_names() {
    CHECK_EQ(std::string(ErrorName(ErrorCode::kNone)), std::string("NONE"));
    CHECK_EQ(std::string(ErrorName(ErrorCode::kNotLeader)),
             std::string("NOT_LEADER"));
    CHECK_EQ(static_cast<int>(ErrorCode::kNotLeader),
             5);  // numbers are wire format
}

// ---------------------------------------------------------------------------
// Request/response over a real connection

void test_call_over_loopback() {
    auto listener = Listener::Listen(0);
    Socket client = ConnectTo("127.0.0.1", listener->port(),
                              std::chrono::milliseconds(2000));
    Socket server;
    listener->Accept(&server, std::chrono::milliseconds(2000));

    // A tiny server: answers each MetadataRequest with one topic per topic
    // asked for. On the third request it replies with the wrong correlation
    // id, which the client must reject.
    std::thread t([&] {
        for (int i = 0; i < 3; ++i) {
            std::vector<uint8_t> payload;
            if (RecvFrame(server, &payload) != IoStatus::kOk) return;
            MessageHeader h{};
            MetadataRequest req;
            if (!PeekHeader(payload, &h) ||
                h.type != MessageType::kMetadataRequest ||
                !DecodeMessage(payload, &req)) {
                return;
            }
            MetadataResponse resp;
            for (const std::string& name : req.topics)
                resp.topics.push_back({name, ErrorCode::kNone, {}});
            const int32_t reply_id =
                (i == 2) ? h.correlation_id + 1 : h.correlation_id;
            if (SendMessage(server, reply_id, resp) != IoStatus::kOk) return;
        }
    });

    MetadataResponse resp;
    CHECK_EQ(Call(client, 1, sample_metadata_request(), &resp),
             CallStatus::kOk);
    CHECK_EQ(resp.topics.size(), size_t{2});

    MetadataRequest one;
    one.topics = {"orders"};
    CHECK_EQ(Call(client, 2, one, &resp), CallStatus::kOk);
    CHECK_EQ(resp.topics.size(), size_t{1});

    CHECK_EQ(Call(client, 3, one, &resp), CallStatus::kBadResponse);
    t.join();

    // The server is gone now: the next call fails with an I/O error.
    server.Close();
    IoStatus io = IoStatus::kOk;
    CHECK_EQ(Call(client, 4, one, &resp, &io), CallStatus::kIoError);
    CHECK(io == IoStatus::kClosed);
}

}  // namespace
}  // namespace tributary

int main() {
    tributary::test_produce_round_trip();
    tributary::test_fetch_round_trip();
    tributary::test_metadata_round_trip();
    tributary::test_empty_messages_round_trip();
    tributary::test_exact_header_and_body_bytes();
    tributary::test_peek_header();
    tributary::test_wrong_type_rejected();
    tributary::test_trailing_bytes_rejected();
    tributary::test_truncated_messages_rejected();
    tributary::test_huge_list_count_rejected_without_allocating();
    tributary::test_unknown_error_code_is_kept();
    tributary::test_error_names();
    tributary::test_call_over_loopback();
    return tributary::ReportResults();
}
