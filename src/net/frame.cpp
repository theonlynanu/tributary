#include "frame.h"

#include <cerrno>
#include <cstdint>
#include <cstring>

#include "common/bytes.h"
#include "net/socket.h"

namespace tributary {

IoStatus SendFrame(Socket& socket, const uint8_t* data, size_t n) {
    if (n > kMaxFrameSize) {
        errno = EMSGSIZE;
        return IoStatus::kError;
    }

    // Length and payload go out in a single send
    std::vector<uint8_t> frame(4 + n);
    detail::store_be(frame.data(), static_cast<uint32_t>(n));
    if (n > 0) {
        std::memcpy(frame.data() + 4, data, n);
    }
    return socket.SendAll(frame.data(), frame.size());
}

IoStatus RecvFrame(Socket& socket, std::vector<uint8_t>* out) {
    uint8_t header[4];
    const IoStatus header_status = socket.RecvAll(header, sizeof(header));
    if (header_status != IoStatus::kOk) return header_status;

    ByteReader reader(header, sizeof(header));
    const uint32_t length = reader.u32();
    if (length > kMaxFrameSize) {
        errno = EMSGSIZE;
        return IoStatus::kError;
    }

    out->resize(length);
    if (length == 0) return IoStatus::kOk;
    return socket.RecvAll(out->data(), length);
}

}  // namespace tributary
