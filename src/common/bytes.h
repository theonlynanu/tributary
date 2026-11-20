#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>
namespace tributary {

/**
 * Builds a buffer by appending fields in network byte order (big endian).
 *
 * Owns its buffer.
 */
class ByteWriter {
   public:
    ByteWriter() = default;
    explicit ByteWriter(std::size_t reserve_bytes);

    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void i16(int16_t v);
    void i32(int32_t v);
    void i64(int64_t v);

    void bytes(const uint8_t* p, size_t n);
    void str(std::string_view s);
    void blob(const uint8_t* p, size_t n);

    /**
     * Overwrites 4 bytes that have previously been written.
     *
     * Requires pos + 4 < size()
     */
    void put_u32_at(size_t pos, uint32_t v);

    size_t size() const;
    const uint8_t* data() const;

    std::vector<uint8_t> take();

   private:
    template <typename T>
    void put_be(T v);

    std::vector<uint8_t> buffer_;
};

/**
 * A view into someone else's buffer. Valid only while that buffer lives.
 */
struct ByteView {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

/**
 * Reads fields from a buffer it does not own.
 *
 * Fails on any short read, returns 0/empty, and stays failed. Check ok() at the
 * end.
 */
class ByteReader {
   public:
    ByteReader(const uint8_t* data, size_t len);
    explicit ByteReader(const std::vector<uint8_t>& buf);
    ByteReader(std::vector<uint8_t>&&) = delete;

    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int16_t i16();
    int32_t i32();
    int64_t i64();

    const uint8_t* bytes(size_t n);
    std::string str();
    ByteView blob();
    void skip(size_t n);

    bool ok() const;
    size_t position() const;
    size_t remaining() const;

   private:
    template <typename T>
    T get_be();
    bool need(size_t n);

    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    size_t ok_ = true;
};

}  // namespace tributary
