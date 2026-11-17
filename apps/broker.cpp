#include <iostream>

#include "common/crc32c.h"

int main(int argc, char* argv[]) {
    std::cout << "tributary broker (stub)\n";
    for (int i = 1; i < argc; i++) {
        std::cout << " arg[" << i << "] = " << argv[i] << "\n";
    }

    const std::string probe = "123456789";
    const uint32_t crc = tributary::crc32c(
        reinterpret_cast<const uint8_t*>(probe.data()), probe.size());
    std::cout << " crc32(\"123456789\") = 0x" << std::hex << crc << std::dec
              << "\n";

    return 0;
}
