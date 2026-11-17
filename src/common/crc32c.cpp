#include "common/crc32c.h"

#include <array>

namespace tributary {

namespace {

/** Castagnoli polynomial 0x1EDC6F41, bit-reversed for LSB-first */
static constexpr uint32_t kPoly = 0x82F63B78;

constexpr std::array<uint32_t, 256> generate_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ kPoly) : (crc >> 1);
        }
        table[i] = crc;
    }
    return table;
}

// Build by compiler, no work happens at runtime
constexpr std::array<uint32_t, 256> kTable = generate_table();

}  // namespace

uint32_t crc32c(const uint8_t* data, size_t len, uint32_t prev) {
    uint32_t crc = ~prev;
    const auto* bytes = static_cast<const uint8_t*>(data);

    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ kTable[(crc ^ bytes[i]) & 0xFFu];
    }

    return ~crc;
}

}  // namespace tributary
