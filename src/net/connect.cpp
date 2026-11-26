#include "connect.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common/unique_fd.h"

namespace tributary {
namespace {
using Clock = std::chrono::steady_clock;

/**
 * Switches socket between blocking and non-blocking mode.
 *
 * Exclusively for connecting, in order to prevent a blocking connect call to a
 * dead/powered off machine from stalling for too long.
 */
bool SetBlocking(int fd, bool blocking) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    const int updated = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, updated) == 0;
}

/**
 * Connects one socket to one address, waiting at most until deadline.
 *
 * Returns 0 on success, or with errno value decribing failure.
 */
int ConnectOne(int fd, const addrinfo* ai, Clock::time_point deadline) {
    // Set connect to non-blocking, which returns with EINPROGRESS.
    if (!SetBlocking(fd, false)) return errno;
    // poll() should wait for it to finish according to defined timeout instead
    // of kernel timeout

    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) return 0;
    if (errno != EINPROGRESS) return errno;

    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());

        if (left.count() <= 0) return ETIMEDOUT;

        pollfd p{};
        p.fd = fd;
        p.events = POLLOUT;
        const int ready = ::poll(&p, 1, static_cast<int>(left.count()));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return errno;
        if (ready == 0) return ETIMEDOUT;
        break;
    }

    // Retrieve and surface any socket errors
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return errno;
    if (err != 0) return err;

    // Set socket back to blocking mode
    if (!SetBlocking(fd, true)) return errno;
    return 0;
}

}  // namespace

Socket ConnectTo(const std::string& host, uint16_t port,
                 std::chrono::milliseconds timeout, std::string* error) {
    const Clock::time_point deadline = Clock::now() + timeout;
    const std::string target = host + ":" + std::to_string(port);

    // Look up name over IPv4
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const std::string port_str = std::to_string(port);
    const int gai =
        ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &results);

    if (gai != 0) {
        if (error) {
            *error = "resolve " + target + ": " + ::gai_strerror(gai);
        }
        return Socket();
    }

    int last_err = ECONNREFUSED;
    // One of the cooler loops I've written
    for (const addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        UniqueFd fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!fd.valid()) {
            last_err = errno;
            continue;
        }

        last_err = ConnectOne(fd.get(), ai, deadline);
        if (last_err == 0) {
            ::freeaddrinfo(results);
            return Socket(fd.release());  // socket connected
        }
        if (last_err == ETIMEDOUT) break;  // overall time used up
    }

    ::freeaddrinfo(results);

    if (error) {
        *error = "connect " + target + ": " + std::strerror(last_err);
    }

    errno = last_err;
    return Socket();
}

}  // namespace tributary
