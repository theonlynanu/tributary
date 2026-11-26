#include "socket.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <cstddef>
#include <ctime>

namespace tributary {
namespace {

// On Linux, MSG_NOSIGNAL makes a send to a closed peer fail with EPIPE instead
// of raising SIGPIPE, which kills the process. macOS does not do this, so I set
// SO_NOSIGPIPE to prevent the process from being killed.
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// Separate local failures from failures from the other side of the socket
bool IsConnectionLost(int err) {
    return err == EPIPE || err == ECONNRESET || err == ENOTCONN ||
           err == ECONNABORTED;
}

bool SetTimeout(int fd, int option, std::chrono::milliseconds timeout) {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    return ::setsockopt(fd, SOL_SOCKET, option, &tv, sizeof(tv)) == 0;
}

}  // namespace

Socket::Socket(int fd) : fd_(fd) {
    if (!fd_.valid()) return;
    SetNoDelay(true);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(fd_.get(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

IoStatus Socket::SendAll(const uint8_t* data, size_t n) {
    size_t done = 0;
    while (done < n) {
        const ssize_t sent =
            ::send(fd_.get(), data + done, n - done, kSendFlags);
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return IoStatus::kTimeout;
            if (IsConnectionLost(errno)) return IoStatus::kClosed;
            return IoStatus::kError;
        }
        done += static_cast<size_t>(sent);
    }
    return IoStatus::kOk;
}

IoStatus Socket::RecvAll(uint8_t* data, size_t n) {
    size_t done = 0;
    while (done < n) {
        const ssize_t got = ::recv(fd_.get(), data + done, n - done, 0);
        if (got == 0) return IoStatus::kClosed;
        if (got < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return IoStatus::kTimeout;
            }
            if (IsConnectionLost(errno)) return IoStatus::kClosed;
            return IoStatus::kError;
        }
        done += static_cast<size_t>(got);
    }
    return IoStatus::kOk;
}

bool Socket::SetRecvTimeout(std::chrono::milliseconds timeout) {
    return SetTimeout(fd_.get(), SO_RCVTIMEO, timeout);
}

bool Socket::SetSendTimeout(std::chrono::milliseconds timeout) {
    return SetTimeout(fd_.get(), SO_SNDTIMEO, timeout);
}

bool Socket::SetNoDelay(bool on) {
    const int value = on ? 1 : 0;
    return ::setsockopt(fd_.get(), IPPROTO_TCP, TCP_NODELAY, &value,
                        sizeof(value)) == 0;
}

void Socket::Shutdown() {
    if (fd_.valid()) {
        ::shutdown(fd_.get(), SHUT_RDWR);
    }
}

}  // namespace tributary
