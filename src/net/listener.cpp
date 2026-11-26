#include "listener.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>

namespace tributary {
namespace {

void SetError(std::string* error, const std::string& what) {
    if (error) {
        *error = what + ": " + std::strerror(errno);
    }
}

}  // namespace

Listener::Listener(UniqueFd fd, uint16_t port)
    : fd_(std::move(fd)), port_(port) {}

std::unique_ptr<Listener> Listener::Listen(uint16_t port, std::string* error) {
    UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));

    if (!fd.valid()) {
        SetError(error, "socket");
        return nullptr;
    }

    const int one = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) !=
        0) {
        SetError(error, "setsockopt(SO_REUSE_ADDR)");
        return nullptr;
    }

    sockaddr_in addr{};  // initialized to all zeroes
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
        0) {
        SetError(error, "bind port " + std::to_string(port));
        return nullptr;
    }

    if (::listen(fd.get(), SOMAXCONN) != 0) {
        SetError(error, "listen");
        return nullptr;
    }

    // If port 0 was passed, get the actual port
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&bound), &len) !=
        0) {
        SetError(error, "getsockname");
        return nullptr;
    }

    return std::unique_ptr<Listener>(
        new Listener(std::move(fd), ntohs(bound.sin_port)));
}

IoStatus Listener::Accept(Socket* out, std::chrono::milliseconds timeout) {
    // Wait until connection is ready or timeout passes
    if (timeout.count() >= 0) {
        pollfd p{};
        p.fd = fd_.get();
        p.events = POLLIN;
        int ready;
        do {
            ready = ::poll(&p, 1, static_cast<int>(timeout.count()));
        } while (ready < 0 && errno == EINTR);
        if (ready < 0) return IoStatus::kError;
        if (ready == 0) return IoStatus::kTimeout;
    }

    int conn;
    do {
        conn = ::accept(fd_.get(), nullptr, nullptr);
    } while (conn < 0 && errno == EINTR);

    if (conn < 0) {
        // Client error
        if (errno == ECONNABORTED || errno == EAGAIN || errno == EWOULDBLOCK) {
            return IoStatus::kTimeout;
        }
        return IoStatus::kError;
    }

    *out = Socket(conn);
    return IoStatus::kOk;
}

}  // namespace tributary
