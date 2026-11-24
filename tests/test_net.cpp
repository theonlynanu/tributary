#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#include "common/bytes.h"
#include "net/connect.h"
#include "net/frame.h"
#include "net/listener.h"
#include "net/socket.h"
#include "test.h"

namespace tributary {

// Lets CHECK_EQ print an IoStatus (it must live in namespace tributary).
std::ostream& operator<<(std::ostream& os, IoStatus s) {
    switch (s) {
        case IoStatus::kOk:
            return os << "kOk";
        case IoStatus::kClosed:
            return os << "kClosed";
        case IoStatus::kTimeout:
            return os << "kTimeout";
        case IoStatus::kError:
            return os << "kError";
    }
    return os << "IoStatus(" << static_cast<int>(s) << ")";
}

namespace {

using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

constexpr milliseconds kConnectTimeout(2000);

// A connected pair of sockets over loopback: `client` from ConnectTo and
// `server` from Accept, exactly as a real client and broker would get them.
struct Pair {
    std::unique_ptr<Listener> listener;
    Socket client;
    Socket server;
};

Pair make_pair() {
    Pair p;
    std::string error;
    p.listener = Listener::Listen(0, &error);
    if (!p.listener) {
        std::cout << "  Listen failed: " << error << "\n";
        return p;
    }
    p.client =
        ConnectTo("127.0.0.1", p.listener->port(), kConnectTimeout, &error);
    if (!p.client.valid()) std::cout << "  ConnectTo failed: " << error << "\n";
    p.listener->Accept(&p.server, milliseconds(2000));
    return p;
}

std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 31 + 7);
    return v;
}

long long elapsed_ms(Clock::time_point start) {
    return std::chrono::duration_cast<milliseconds>(Clock::now() - start)
        .count();
}

// ---------------------------------------------------------------------------
// Connecting and accepting

void test_listen_on_any_free_port() {
    auto a = Listener::Listen(0);
    auto b = Listener::Listen(0);
    CHECK(a != nullptr && b != nullptr);
    if (a && b) {
        CHECK(a->port() != 0);
        CHECK(a->port() != b->port());
    }
}

void test_listen_on_a_taken_port_fails() {
    auto a = Listener::Listen(0);
    std::string error;
    CHECK(Listener::Listen(a->port(), &error) == nullptr);
    CHECK(!error.empty());
}

void test_connect_and_accept() {
    Pair p = make_pair();
    CHECK(p.client.valid());
    CHECK(p.server.valid());
}

void test_connect_by_host_name() {
    auto listener = Listener::Listen(0);
    Socket s = ConnectTo("localhost", listener->port(), kConnectTimeout);
    CHECK(s.valid());
}

void test_connected_sockets_have_nodelay() {
    Pair p = make_pair();
    for (const Socket* s : {&p.client, &p.server}) {
        int value = 0;
        socklen_t len = sizeof(value);
        CHECK_EQ(::getsockopt(s->fd(), IPPROTO_TCP, TCP_NODELAY, &value, &len),
                 0);
        CHECK(value != 0);
    }
}

void test_connect_refused_fails_fast() {
    // Find a free port, then stop listening on it.
    uint16_t port = 0;
    {
        port = Listener::Listen(0)->port();
    }

    const auto start = Clock::now();
    std::string error;
    Socket s = ConnectTo("127.0.0.1", port, kConnectTimeout, &error);
    CHECK(!s.valid());
    CHECK_EQ(errno, ECONNREFUSED);
    CHECK(!error.empty());
    CHECK(elapsed_ms(start) < 1000);
}

void test_unknown_host_fails() {
    std::string error;
    Socket s =
        ConnectTo("no-such-host.invalid", 41100, kConnectTimeout, &error);
    CHECK(!s.valid());
    CHECK(error.find("resolve") != std::string::npos);
}

void test_accept_times_out() {
    auto listener = Listener::Listen(0);
    Socket s;
    const auto start = Clock::now();
    CHECK_EQ(listener->Accept(&s, milliseconds(100)), IoStatus::kTimeout);
    CHECK(!s.valid());
    CHECK(elapsed_ms(start) >= 90);
}

// ---------------------------------------------------------------------------
// Frames

void test_frames_of_various_sizes() {
    Pair p = make_pair();
    for (size_t n : {size_t{0}, size_t{1}, size_t{1000}, size_t{1} << 20}) {
        const std::vector<uint8_t> sent = pattern(n);

        // A 1 MiB frame doesn't fit in the socket buffers, so sender and
        // receiver must run at the same time, as across a real network.
        IoStatus send_status = IoStatus::kError;
        std::thread sender([&] { send_status = SendFrame(p.client, sent); });
        std::vector<uint8_t> got = {42};
        CHECK_EQ(RecvFrame(p.server, &got), IoStatus::kOk);
        sender.join();

        CHECK_EQ(send_status, IoStatus::kOk);
        CHECK_EQ(got.size(), n);
        CHECK(got == sent);
    }
}

void test_back_to_back_frames_stay_separate() {
    Pair p = make_pair();
    // Three sends may arrive as one chunk; framing must split them again.
    CHECK_EQ(SendFrame(p.client, pattern(5)), IoStatus::kOk);
    CHECK_EQ(SendFrame(p.client, pattern(0)), IoStatus::kOk);
    CHECK_EQ(SendFrame(p.client, pattern(300)), IoStatus::kOk);

    std::vector<uint8_t> got;
    CHECK_EQ(RecvFrame(p.server, &got), IoStatus::kOk);
    CHECK(got == pattern(5));
    CHECK_EQ(RecvFrame(p.server, &got), IoStatus::kOk);
    CHECK(got.empty());
    CHECK_EQ(RecvFrame(p.server, &got), IoStatus::kOk);
    CHECK(got == pattern(300));
}

void test_request_response_both_ways() {
    Pair p = make_pair();
    std::thread server([&] {
        std::vector<uint8_t> req;
        for (int i = 0; i < 100; ++i) {
            if (RecvFrame(p.server, &req) != IoStatus::kOk) return;
            req.push_back(0xFF);  // reply = request + one byte
            if (SendFrame(p.server, req) != IoStatus::kOk) return;
        }
    });
    bool all_ok = true;
    for (int i = 0; i < 100; ++i) {
        std::vector<uint8_t> req = pattern(static_cast<size_t>(i)), resp;
        all_ok &= SendFrame(p.client, req) == IoStatus::kOk;
        all_ok &= RecvFrame(p.client, &resp) == IoStatus::kOk;
        req.push_back(0xFF);
        all_ok &= resp == req;
    }
    server.join();
    CHECK(all_ok);
}

void test_oversized_frames_rejected() {
    Pair p = make_pair();

    // Sending: refused before anything goes on the wire.
    std::vector<uint8_t> huge(kMaxFrameSize + 1);
    CHECK_EQ(SendFrame(p.client, huge), IoStatus::kError);
    CHECK_EQ(errno, EMSGSIZE);

    // Receiving: a peer claims a 4 GiB frame. Nothing is allocated.
    const uint8_t bogus[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_EQ(p.client.SendAll(bogus, 4), IoStatus::kOk);
    std::vector<uint8_t> got;
    CHECK_EQ(RecvFrame(p.server, &got), IoStatus::kError);
    CHECK_EQ(errno, EMSGSIZE);
    CHECK(got.capacity() < 1024);
}

// ---------------------------------------------------------------------------
// Closing, timeouts and shutdown

void test_peer_close_before_frame() {
    Pair p = make_pair();
    p.client.Close();
    std::vector<uint8_t> got;
    CHECK_EQ(RecvFrame(p.server, &got), IoStatus::kClosed);
}

void test_peer_close_mid_frame() {
    Pair p = make_pair();
    // Header promises 100 bytes; only 10 arrive before the close.
    ByteWriter w;
    w.u32(100);
    const std::vector<uint8_t> partial = pattern(10);
    w.bytes(partial.data(), partial.size());
    const std::vector<uint8_t> bytes = w.take();
    CHECK_EQ(p.client.SendAll(bytes.data(), bytes.size()), IoStatus::kOk);
    p.client.Close();

    std::vector<uint8_t> got;
    CHECK_EQ(RecvFrame(p.server, &got), IoStatus::kClosed);
}

void test_recv_timeout() {
    Pair p = make_pair();
    CHECK(p.server.SetRecvTimeout(milliseconds(100)));

    const auto start = Clock::now();
    std::vector<uint8_t> got;
    CHECK_EQ(RecvFrame(p.server, &got), IoStatus::kTimeout);
    CHECK(elapsed_ms(start) >= 90);
}

void test_send_to_closed_peer_does_not_kill_process() {
    // Without MSG_NOSIGNAL / SO_NOSIGPIPE this test would end the whole
    // test program with SIGPIPE instead of returning kClosed.
    Pair p = make_pair();
    p.server.Close();

    const std::vector<uint8_t> chunk = pattern(64 * 1024);
    IoStatus status = IoStatus::kOk;
    for (int i = 0; i < 1000 && status == IoStatus::kOk; ++i) {
        status = SendFrame(p.client, chunk);
    }
    CHECK_EQ(status, IoStatus::kClosed);
}

void test_shutdown_wakes_blocked_receiver() {
    Pair p = make_pair();
    std::atomic<bool> returned{false};
    IoStatus status = IoStatus::kOk;

    std::thread reader([&] {
        std::vector<uint8_t> got;
        status = RecvFrame(p.server, &got);  // nothing will ever arrive
        returned = true;
    });

    std::this_thread::sleep_for(milliseconds(100));
    CHECK(!returned.load());
    p.server.Shutdown();  // from another thread, as a stopping broker does
    reader.join();

    CHECK_EQ(status, IoStatus::kClosed);
}

void test_socket_moves() {
    Pair p = make_pair();
    const int fd = p.client.fd();
    Socket moved = std::move(p.client);
    CHECK(!p.client.valid());
    CHECK_EQ(moved.fd(), fd);
    CHECK_EQ(SendFrame(moved, pattern(3)), IoStatus::kOk);
    std::vector<uint8_t> got;
    CHECK_EQ(RecvFrame(p.server, &got), IoStatus::kOk);
}

}  // namespace
}  // namespace tributary

int main() {
    tributary::test_listen_on_any_free_port();
    tributary::test_listen_on_a_taken_port_fails();
    tributary::test_connect_and_accept();
    tributary::test_connect_by_host_name();
    tributary::test_connected_sockets_have_nodelay();
    tributary::test_connect_refused_fails_fast();
    tributary::test_unknown_host_fails();
    tributary::test_accept_times_out();
    tributary::test_frames_of_various_sizes();
    tributary::test_back_to_back_frames_stay_separate();
    tributary::test_request_response_both_ways();
    tributary::test_oversized_frames_rejected();
    tributary::test_peer_close_before_frame();
    tributary::test_peer_close_mid_frame();
    tributary::test_recv_timeout();
    tributary::test_send_to_closed_peer_does_not_kill_process();
    tributary::test_shutdown_wakes_blocked_receiver();
    tributary::test_socket_moves();
    return tributary::ReportResults();
}
