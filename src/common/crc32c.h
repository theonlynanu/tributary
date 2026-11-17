#pragma once

#include <cstddef>
#include <cstdint>

namespace tributary {

/**
 * Returns the CRC32 of data[0..len).
 *
 * To checksum data that arrives in pieces, pass the previous result as'prev'
 *   uint32_t c = crc32c(a, a_len);
 *   c = crc32c(b, b_len, c);
 */
uint32_t crc32c(const uint8_t* data, size_t len, uint32_t prev = 0);

}  // namespace tributary
