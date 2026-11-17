#pragma once

#include <cstdint>
#include <iostream>
#include <type_traits>
namespace tributary {

// Counters shared by checks
inline int g_test_checks = 0;
inline int g_test_failures = 0;

// Prints a value in a failure message. Overloads will print single-byte
// integers as numbers instead of characters and bools as true/false
template <typename T>
void PrintValue(std::ostream& os, const T& v) {
    os << v;
}
inline void PrintValue(std::ostream& os, uint8_t v) {
    os << static_cast<unsigned>(v);
}
inline void PrintValue(std::ostream& os, int8_t v) {
    os << static_cast<int>(v);
}
inline void PrintValue(std::ostream& os, bool v) {
    os << (v ? "true" : "false");
}

// Prints an integer in hexadecimal
template <typename T>
void PrintHex(std::ostream& os, const T& v) {
    static_assert(std::is_integral<T>::value,
                  "CHECK_EQ_HEX requires an integer");
    os << "0x" << std::hex << static_cast<unsigned long long>(v) << std::dec;
}

inline int ReportResults() {
    if (g_test_failures == 0) {
        std::cout << "OK (" << g_test_checks << " checks)\n";
        return 0;
    }
    std::cout << g_test_failures << " of " << g_test_checks
              << " checks FAILED\n";
    return 1;
}

}  // namespace tributary

/**
 * Shared implementation that evaluates each argument exactly once so that calls
 * like 'CHECK_EQ(reader.u32(), 0x12345678u);' don't execute function calls
 * multiple times
 */
#define CHECK_IMPL(actual, expected, print)                               \
    do {                                                                  \
        const auto& actual_ = (actual);                                   \
        const auto& expected_ = (expected);                               \
        ::tributary::g_test_checks++;                                     \
        if (!(actual_ == expected_)) {                                    \
            ::tributary::g_test_failures++;                               \
            std::cout << "FAIL " << __FILE__ << ":" << "__LINE__" << ": " \
                      << #actual << " = ";                                \
            print(std::cout, actual_);                                    \
            std::cout << ", expected ";                                   \
            print(std::cout, expected_);                                  \
            std::cout << "\n";                                            \
        }                                                                 \
    } while (0)

#define CHECK_EQ(actual, expected) \
    CHECK_IMPL(actual, expected, ::tributary::PrintValue)
#define CHECK_EQ_HEX(actual, expected) \
    CHECK_IMPL(actual, expected, ::tributary::PrintHex)
#define CHECK(cond) CHECK_EQ(static_cast<bool>(cond), true)
