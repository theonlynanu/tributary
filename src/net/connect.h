/**
 * Opening a connection to a server by hostname and port.
 */

#include <chrono>
#include <cstdint>
#include <string>

#include "net/socket.h"

namespace tributary {

Socket ConnectTo(const std::string& host, uint16_t port,
                 std::chrono::milliseconds timeout,
                 std::string* error = nullptr);

}
