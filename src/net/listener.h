/**
 * A listening TCP socket that accepts incoming connections.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include "common/unique_fd.h"
#include "net/socket.h"

namespace tributary {

class Listener {
   public:
    /**
     * Listens on port on every local IPv4 address.
     *
     * SO_REUSEADDR is set, so a restarted server can bind again quickly without
     * requiring the old connection to drain. Pass port 0 to let the system
     * choose a free port, and get that port with port().
     *
     * Returns nullptr on failure with description in *error.
     */
    static std::unique_ptr<Listener> Listen(uint16_t port,
                                            std::string* error = nullptr);

    Listener(const Listener&) = delete;             // No copy
    Listener& operator=(const Listener&) = delete;  // No copy assignment
    /**
     * Get the bound port.
     */
    uint16_t port() const { return port_; }

    /**
     * Waits for a connection and stores it in *out.
     *
     * Negative timeout waits forever. Returns kOk with a connected Socket,
     * kTimeout if no connection arrives in time, or kError.
     */
    IoStatus Accept(Socket* out, std::chrono::milliseconds timeout =
                                     std::chrono::milliseconds(-1));

   private:
    Listener(UniqueFd fd, uint16_t port);

    UniqueFd fd_;
    uint16_t port_;
};

}  // namespace tributary
