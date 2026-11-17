#include <cstdint>
#include <string>
#include <vector>

#include "common/crc32c.h"
#include "test.h"

namespace tributary {
namespace {

uint32_t crc_of(const std::string& s) {
    return crc32c(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

uint32_t crc_of(const std::vector<uint8_t>& v) {
    return crc32c(v.data(), v.size());
}

// The standard CRC32C check value.
void test_check_value() {
    CHECK_EQ_HEX(crc_of(std::string("123456789")), 0xE3069283u);
}

void test_empty_input() {
    CHECK_EQ_HEX(crc32c(nullptr, 0), 0u);
    CHECK_EQ_HEX(crc_of(std::string()), 0u);
}

// Test vectors from RFC 3720 (iSCSI), Appendix B.4.
void test_rfc3720_vectors() {
    std::vector<uint8_t> zeros(32, 0x00);
    std::vector<uint8_t> ones(32, 0xFF);
    std::vector<uint8_t> incrementing(32);
    for (size_t i = 0; i < incrementing.size(); ++i) {
        incrementing[i] = static_cast<uint8_t>(i);
    }

    CHECK_EQ_HEX(crc_of(zeros), 0x8A9136AAu);
    CHECK_EQ_HEX(crc_of(ones), 0x62A8AB43u);
    CHECK_EQ_HEX(crc_of(incrementing), 0x46DD794Eu);
}

// Checksumming in two pieces must match checksumming all at once,
// at every possible split point (including empty first or second piece).
void test_incremental_matches_whole() {
    const std::string whole = "123456789";
    const auto* p = reinterpret_cast<const uint8_t*>(whole.data());

    for (size_t split = 0; split <= whole.size(); ++split) {
        uint32_t crc = crc32c(p, split);
        crc = crc32c(p + split, whole.size() - split, crc);
        CHECK_EQ_HEX(crc, 0xE3069283u);
    }
}

// Every single-bit change must change the checksum. Recovery relies on this
// to detect torn or corrupted batches.
void test_single_bit_flips_detected() {
    std::vector<uint8_t> data(64);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i * 37);
    }
    const uint32_t original = crc_of(data);

    for (size_t byte = 0; byte < data.size(); ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            data[byte] ^= static_cast<uint8_t>(1u << bit);
            CHECK(crc_of(data) != original);
            data[byte] ^= static_cast<uint8_t>(1u << bit);  // restore
        }
    }
}

}  // namespace
}  // namespace tributary

int main() {
    tributary::test_check_value();
    tributary::test_empty_input();
    tributary::test_rfc3720_vectors();
    tributary::test_incremental_matches_whole();
    tributary::test_single_bit_flips_detected();
    return tributary::ReportResults();
}
