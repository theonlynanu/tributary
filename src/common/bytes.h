#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace tributary {

namespace detail {

/**
 * Writes v to dst[0..sizeof(T)] in network (big endian) order.
 *
 * dst must already have room for sizeof(T) bytes.
 */
template <typename T>
void store_be(uint8_t* dst, T v) {
    static_assert(std::is_unsigned_v<T>, "store_be requires an unsigned type");
    for (size_t i = 0; i < sizeof(T); i++) {
        dst[i] = static_cast<uint8_t>(v >> (8 * (sizeof(T) - 1 - i)));
    }
}

}  // namespace detail

/**
 * Builds a buffer by appending fields in network byte order (big endian).
 *
 * Owns its buffer.
 */
class ByteWriter {
   public:
    ByteWriter() = default;

    /**
     * Set aside memory up front to prevent frequent reallocation.
     */
    explicit ByteWriter(size_t reserve_bytes) {
        buffer_.reserve(reserve_bytes);
    }

    /**
     * Append a uint8_t to ByteWriter's buffer.
     */
    void u8(uint8_t v) { buffer_.push_back(v); }

    /**
     * Append a uint16_t to ByteWriter's buffer.
     */
    void u16(uint16_t v) { put_be(v); }

    /**
     * Append a uint32_t to ByteWriter's buffer.
     */
    void u32(uint32_t v) { put_be(v); }

    /**
     * Append a uint64_t to ByteWriter's buffer.
     */
    void u64(uint64_t v) { put_be(v); }

    /**
     * Append an int16_t to ByteWriter's buffer.
     */
    void i16(int16_t v) { put_be(static_cast<uint16_t>(v)); }

    /**
     * Append an int32_t to ByteWriter's buffer.
     */
    void i32(int32_t v) { put_be(static_cast<uint32_t>(v)); }

    /**
     * Append an int64_t to ByteWriter's buffer.
     */
    void i64(int64_t v) { put_be(static_cast<uint64_t>(v)); }

    /**
     * Appends n raw bytes with no length in front.
     */
    void bytes(const uint8_t* p, size_t n) {
        if (n == 0) return;
        buffer_.insert(buffer_.end(), p, p + n);
    }

    /**
     * Appends a string with 2-byte length prefix.
     */
    void str(std::string_view s) {
        // Append 2-byte length prefix
        assert(s.size() <= std::numeric_limits<uint16_t>::max());
        u16(static_cast<uint16_t>(s.size()));

        // Append characters
        bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    /**
     * Appends a blob of length n with a 4-byte length prefix.
     */
    void blob(const uint8_t* p, size_t n) {
        // Append 4-byte length prefix
        assert(n <= std::numeric_limits<uint32_t>::max());
        u32(static_cast<uint32_t>(n));

        // Append blob
        bytes(p, n);
    }

    /**
     * Overwrites 4 bytes that have previously been written.
     *
     * Requires pos + 4 <= size()
     */
    void put_u32_at(size_t pos, uint32_t v) {
        assert(pos <= buffer_.size() && buffer_.size() - pos >= 4);
        detail::store_be(buffer_.data() + pos, v);
    }

    size_t size() const { return buffer_.size(); }
    const uint8_t* data() const { return buffer_.data(); }

    /**
     * Moves the buffer out without copying it, then clears the ByteWriter's
     * buffer.
     */
    std::vector<uint8_t> take() {
        std::vector<uint8_t> out = std::move(buffer_);
        buffer_.clear();
        return out;
    }

   private:
    /**
     * Appends an unsigned value in network byte (big endian) order.
     */
    template <typename T>
    void put_be(T v) {
        const size_t old_size = buffer_.size();
        buffer_.resize(old_size + sizeof(T));
        detail::store_be(buffer_.data() + old_size, v);
    }

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
 * Fails on any short read, returns 0/empty without moving the read position.
 * Once failed, the reader will always fail to prevent continued misreads. Check
 * ok() at the end of a read sequence.
 */
class ByteReader {
   public:
    ByteReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}
    explicit ByteReader(const std::vector<uint8_t>& buf)
        : ByteReader(buf.data(), buf.size()) {}
    ByteReader(std::vector<uint8_t>&&) = delete;

    /**
     * Reads a uint8_t from the buffer, if possible.
     */
    uint8_t u8() {
        if (!need(1)) return 0;
        return data_[pos_++];
    }

    /**
     * Reads a uint16_t from the buffer, if possible.
     */
    uint16_t u16() { return get_be<uint16_t>(); }

    /**
     * Reads a uint32_t from the buffer, if possible.
     */
    uint32_t u32() { return get_be<uint32_t>(); }

    /**
     * Reads a uint64_t from the buffer, if possible.
     */
    uint64_t u64() { return get_be<uint64_t>(); }

    /**
     * Reads an int16_t from the buffer, if possible.
     */
    int16_t i16() { return static_cast<int16_t>(get_be<uint16_t>()); }

    /**
     * Reads an int32_t from the buffer, if possible.
     */
    int32_t i32() { return static_cast<int32_t>(get_be<uint32_t>()); }

    /**
     * Reads an int64_t from the buffer, if possible.
     */
    int64_t i64() { return static_cast<int64_t>(get_be<uint64_t>()); }

    /**
     * Returns a pointer to the next n bytes.
     *
     * Check ok(), since a non-null pointer is not always a successful read.
     */
    const uint8_t* bytes(size_t n) {
        if (!need(n)) return nullptr;
        auto* out = data_ + pos_;
        pos_ += n;
        return out;
    }

    /**
     * Reads a string from buffer, assuming it has a 2-byte length prefix.
     */
    std::string str() {
        uint16_t n = u16();
        const uint8_t* p = bytes(n);
        if (p == nullptr) return {};

        return std::string(reinterpret_cast<const char*>(p), n);
    }

    /**
     * Reads a blob from buffer, assuming it has a 4-byte length prefix.
     *
     * Returns a ByteView into the buffer, which is only valid while the source
     * buffer is.
     */
    ByteView blob() {
        uint32_t n = u32();
        const uint8_t* p = bytes(n);
        if (p == nullptr) return {};
        return ByteView{p, n};
    }

    /**
     * Skips over n bytes that do not need to be decoded.
     */
    void skip(size_t n) {
        if (need(n)) pos_ += n;
    }

    /**
     * Returns whether reads so far have been valid. Returns false if any
     * previous read attempted to read bytes beyond buffer length.
     */
    bool ok() const { return ok_; }

    size_t position() const { return pos_; }
    size_t remaining() const { return len_ - pos_; }

   private:
    /**
     * Read sizeof(T) bytes as a network (big endian) unsigned value
     */
    template <typename T>
    T get_be() {
        static_assert(std::is_unsigned_v<T>,
                      "get_be can only read unsigned values");

        if (!need(sizeof(T))) return 0;

        T v = 0;
        for (size_t i = 0; i < sizeof(T); i++) {
            v = static_cast<T>((v << 8) | data_[pos_ + i]);
        }

        pos_ += sizeof(T);
        return v;
    }

    /**
     * Checks if n more bytes can be read from the buffer without moving read
     * position.
     */
    bool need(size_t n) {
        if (!ok_) return false;

        if (n > len_ - pos_) {
            ok_ = false;
            return false;
        }

        return true;
    }

    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace tributary
