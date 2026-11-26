/**
 * Length-prefixed frames over a Socket.
 *
 * TCP stream has no message boundaries. Since Tributary's messages vary in
 * size, each one is made to travel as a frame, with the first 4 bytes being a
 * u32 that represents the number of payload bytes that follow.
 *
 * Receiver reads 4-byte length and reads exactly that many bytes.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "net/socket.h"

namespace tributary {

/**
 * Largest accepted payload. Since max batch size is 16MiB, this leaves room for
 * several to be in a single payload.
 */
constexpr uint32_t kMaxFrameSize = 64 * 1024 * 1024;

/**
 * Sends data[0..n) as a single frame.
 *
 * Returns kError with errno and sends nothing if n exceeds kMaxFrameSize.
 * Anything other than kOk means the connection should be closed.
 */
IoStatus SendFrame(Socket& socket, const uint8_t* data, size_t n);

/**
 * Sends payload as a single frame.
 *
 * Returns kError with errno and sends nothing if n exceeds kMaxFrameSize.
 * Anything other than kOk means the connection should be closed.
 */
inline IoStatus SendFrame(Socket& socket, const std::vector<uint8_t>& payload) {
    return SendFrame(socket, payload.data(), payload.size());
}

/**
 * Receives one frame's payload into out, replacing its contents.
 *
 * Returns kClosed if peer closes connection, kTimeout if receive timeout
 * expires, and kError with errno EMSGSIZE if length prefix exceeds
 * kMaxFrameSize.
 *
 * Any return but kOk means the connection should be closed.
 */
IoStatus RecvFrame(Socket& socket, std::vector<uint8_t>* out);

}  // namespace tributary
