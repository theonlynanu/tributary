#include "segment.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <utility>

namespace tributary {

namespace {

constexpr size_t kSegmentNameDigits = 20;
constexpr char kSegmentSuffix[] = ".log";
constexpr size_t kSegmentSuffixLen = sizeof(kSegmentSuffix) - 1;

std::string JoinPath(const std::string& dir, const std::string& name) {
    return (std::filesystem::path(dir) / name).string();
}

}  // namespace

std::string SegmentFileName(int64_t base_offset) {
    assert(base_offset >= 0);

    // 20 digits + ".log" + "\0"
    char buf[kSegmentNameDigits + kSegmentSuffixLen + 1];
    std::snprintf(buf, sizeof(buf), "%020" PRId64 ".log", base_offset);
    return std::string(buf);
}

bool ParseSegmentFileName(const std::string& name, int64_t* base_offset) {
    if (name.size() != kSegmentNameDigits + kSegmentSuffixLen) return false;
    if (name.compare(kSegmentNameDigits, kSegmentSuffixLen, kSegmentSuffix) !=
        0) {
        return false;
    }

    // Parse by hand since 20 digits can exceed an int64. I didn't use
    // std::stoll here becasue errant files could cause crashes, and it allows
    // leading symbols, which I want to eliminate
    constexpr uint64_t kMax =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    uint64_t value = 0;

    for (size_t i = 0; i < kSegmentNameDigits; i++) {
        const char c = name[i];
        if (c < '0' || c > '9') return false;

        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (kMax - digit) / 10) return false;  // value*10+digit > kmax
        value = value * 10 + digit;
    }

    *base_offset = static_cast<int64_t>(value);
    return true;
}

Segment::Segment(std::string path, UniqueFd fd, int64_t base_offset,
                 uint64_t size)
    : path_(std::move(path)),
      fd_(std::move(fd)),
      base_offset_(base_offset),
      size_(size) {}

std::unique_ptr<Segment> Segment::Create(const std::string& dir,
                                         int64_t base_offset) {
    std::string path = JoinPath(dir, SegmentFileName(base_offset));

    UniqueFd fd = OpenFile(path, O_RDWR | O_CREAT | O_EXCL);
    if (!fd.valid()) return nullptr;

    // Make new directory durable, or remove it if failed so that retries aren't
    // blocked due to O_EXCL
    if (!SyncDir(dir)) {
        const int saved = errno;
        ::unlink(path.c_str());
        errno = saved;
        return nullptr;
    }

    return std::unique_ptr<Segment>(
        new Segment(std::move(path), std::move(fd), base_offset, 0));
}

std::unique_ptr<Segment> Segment::OpenExisting(const std::string& dir,
                                               int64_t base_offset) {
    std::string path = JoinPath(dir, SegmentFileName(base_offset));

    UniqueFd fd = OpenFile(path, O_RDWR);
    if (!fd.valid()) return nullptr;

    struct stat st;
    if (::fstat(fd.get(), &st) != 0) return nullptr;

    return std::unique_ptr<Segment>(
        new Segment(std::move(path), std::move(fd), base_offset,
                    static_cast<uint64_t>(st.st_size)));
}

bool Segment::Append(const uint8_t* data, size_t n) {
    if (WriteAll(fd_.get(), data, n, size_)) {
        size_ += n;
        return true;
    }

    const int saved = errno;
    if (::ftruncate(fd_.get(), static_cast<off_t>(size_)) != 0) {
        // Don't think anything actually needs to be here
    }
    errno = saved;
    return false;
}

bool Segment::ReadAt(uint64_t position, size_t n,
                     std::vector<uint8_t>* out) const {
    if (position > size_ || n > size_ - position) return false;
    out->resize(n);
    return ReadAll(fd_.get(), out->data(), n, position);
}

bool Segment::Truncate(uint64_t new_size) {
    if (new_size > size_) return false;
    if (::ftruncate(fd_.get(), static_cast<off_t>(new_size)) != 0) return false;
    size_ = new_size;
    return true;
}

bool Segment::Sync() { return SyncFile(fd_.get()); }

ScanResult Segment::Scan(uint64_t from_position, int64_t expected_offset,
                         bool verify_crc, const ScanFn& fn) const {
    uint64_t pos = from_position;
    int64_t expected = expected_offset;
    std::vector<uint8_t> batch;

    while (pos < size_) {
        const auto stop_here = [&](ScanStop why) {
            return ScanResult{pos, expected, why};
        };

        // Header
        if (size_ - pos < kBatchHeaderSize) {
            return stop_here(ScanStop::kIncomplete);
        }

        uint8_t header_bytes[kBatchHeaderSize];
        if (!ReadAll(fd_.get(), header_bytes, kBatchHeaderSize, pos)) {
            // errno 0 means EOF, file shrank (torn)
            return stop_here(errno == 0 ? ScanStop::kIncomplete
                                        : ScanStop::kIoError);
        }

        BatchHeader h{};

        const BatchStatus status =
            ReadBatchHeader(header_bytes, kBatchHeaderSize, &h);
        if (status != BatchStatus::kOk) return stop_here(ScanStop::kCorrupt);

        // Batch size
        if (h.total_size() > size_ - pos)
            return stop_here(ScanStop::kIncomplete);

        // Offset alignment
        if (h.base_offset != expected) {
            return stop_here(ScanStop::kOffsetMismatch);
        }

        // Does every byte match checksum?
        if (verify_crc) {
            batch.resize(h.total_size());
            if (!ReadAll(fd_.get(), batch.data(), batch.size(), pos)) {
                return stop_here(errno == 0 ? ScanStop::kIncomplete
                                            : ScanStop::kIoError);
            }
            BatchHeader checked{};
            if (CheckBatch(batch.data(), batch.size(), &checked) !=
                BatchStatus::kOk) {
                return stop_here(ScanStop::kCorrupt);
            }
        }

        if (fn && !fn(h, pos)) return stop_here(ScanStop::kStoppedByCallback);
        pos += h.total_size();
        expected = h.next_offset();
    }

    return ScanResult{pos, expected, ScanStop::kEndOfSegment};
}

}  // namespace tributary
