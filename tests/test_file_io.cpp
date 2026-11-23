#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "storage/file_io.h"
#include "test.h"
#include "test_util.h"

namespace tributary {
namespace {

// True if `fd` is an open descriptor in this process.
bool is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

void test_write_then_read() {
    TempDir dir;
    UniqueFd fd = OpenFile(dir.file("data"), O_RDWR | O_CREAT);
    CHECK(fd.valid());

    const std::vector<uint8_t> first = {1, 2, 3, 4};
    const std::vector<uint8_t> second = {9, 8, 7};
    CHECK(WriteAll(fd.get(), first.data(), first.size(), 0));
    CHECK(WriteAll(fd.get(), second.data(), second.size(), 4));
    CHECK_EQ(FileSize(dir.file("data")), uint64_t{7});

    std::vector<uint8_t> got(3);
    CHECK(ReadAll(fd.get(), got.data(), got.size(), 4));
    CHECK(got == second);

    // Overwrite in the middle without changing the size.
    const uint8_t patch = 0xAA;
    CHECK(WriteAll(fd.get(), &patch, 1, 2));
    CHECK(ReadFileBytes(dir.file("data")) ==
          std::vector<uint8_t>({1, 2, 0xAA, 4, 9, 8, 7}));
}

void test_zero_length_io() {
    TempDir dir;
    UniqueFd fd = OpenFile(dir.file("data"), O_RDWR | O_CREAT);
    CHECK(WriteAll(fd.get(), nullptr, 0, 0));
    CHECK(ReadAll(fd.get(), nullptr, 0, 0));
    CHECK_EQ(FileSize(dir.file("data")), uint64_t{0});
}

void test_read_past_end_is_eof_not_error() {
    TempDir dir;
    WriteFileBytes(dir.file("data"), {1, 2, 3});
    UniqueFd fd = OpenFile(dir.file("data"), O_RDONLY);

    std::vector<uint8_t> got(4);
    errno = 12345;
    CHECK(!ReadAll(fd.get(), got.data(), got.size(), 0));
    CHECK_EQ(errno, 0);

    CHECK(!ReadAll(fd.get(), got.data(), 1, 10));
    CHECK_EQ(errno, 0);
}

void test_open_flags() {
    TempDir dir;
    const std::string path = dir.file("data");

    // O_EXCL refuses a file that already exists.
    CHECK(OpenFile(path, O_RDWR | O_CREAT | O_EXCL).valid());
    UniqueFd again = OpenFile(path, O_RDWR | O_CREAT | O_EXCL);
    CHECK(!again.valid());
    CHECK_EQ(errno, EEXIST);

    // Opening a missing file without O_CREAT fails with ENOENT.
    UniqueFd missing = OpenFile(dir.file("nope"), O_RDONLY);
    CHECK(!missing.valid());
    CHECK_EQ(errno, ENOENT);

    // O_CLOEXEC is added automatically.
    UniqueFd fd = OpenFile(path, O_RDONLY);
    CHECK((::fcntl(fd.get(), F_GETFD) & FD_CLOEXEC) != 0);
}

void test_unique_fd_ownership() {
    TempDir dir;
    int raw = -1;
    {
        UniqueFd a = OpenFile(dir.file("data"), O_RDWR | O_CREAT);
        raw = a.get();
        CHECK(a.valid());

        UniqueFd b = std::move(a);  // move construction
        CHECK(!a.valid());
        CHECK_EQ(b.get(), raw);
        CHECK(is_open(raw));

        UniqueFd c;
        c = std::move(b);  // move assignment
        CHECK(!b.valid());
        CHECK_EQ(c.get(), raw);
        CHECK(is_open(raw));
    }  // c destroyed here
    CHECK(!is_open(raw));

    // reset() closes the old descriptor.
    UniqueFd d = OpenFile(dir.file("data"), O_RDONLY);
    const int old = d.get();
    d.reset();
    CHECK(!d.valid());
    CHECK(!is_open(old));

    // release() hands the descriptor back without closing it.
    UniqueFd e = OpenFile(dir.file("data"), O_RDONLY);
    const int released = e.release();
    CHECK(!e.valid());
    CHECK(is_open(released));
    ::close(released);
}

void test_write_file_atomic() {
    TempDir dir;
    const std::string path = dir.file("state");

    CHECK(WriteFileAtomic(path, {1, 2, 3}));
    CHECK(ReadFileBytes(path) == std::vector<uint8_t>({1, 2, 3}));

    // A shorter replacement fully replaces the old contents.
    CHECK(WriteFileAtomic(path, {7}));
    CHECK(ReadFileBytes(path) == std::vector<uint8_t>({7}));

    // No temporary file is left behind.
    CHECK(ListFiles(dir.path()) == std::vector<std::string>{"state"});
}

void test_write_file_atomic_fails_cleanly() {
    TempDir dir;
    // The parent directory does not exist, so the temp file can't be made.
    CHECK(!WriteFileAtomic(dir.file("missing/state"), {1}));
    CHECK(ListFiles(dir.path()).empty());
}

void test_read_whole_file() {
    TempDir dir;
    WriteFileBytes(dir.file("data"), {5, 6, 7, 8});

    std::vector<uint8_t> out = {99, 99};  // replaced, not appended to
    CHECK(ReadWholeFile(dir.file("data"), &out));
    CHECK(out == std::vector<uint8_t>({5, 6, 7, 8}));

    WriteFileBytes(dir.file("empty"), {});
    CHECK(ReadWholeFile(dir.file("empty"), &out));
    CHECK(out.empty());

    CHECK(!ReadWholeFile(dir.file("missing"), &out));
    CHECK_EQ(errno, ENOENT);
}

void test_sync() {
    TempDir dir;
    UniqueFd fd = OpenFile(dir.file("data"), O_RDWR | O_CREAT);
    const uint8_t b = 1;
    CHECK(WriteAll(fd.get(), &b, 1, 0));
    CHECK(SyncFile(fd.get()));
    CHECK(SyncDir(dir.path()));
    CHECK(!SyncDir(dir.file("missing")));
}

}  // namespace
}  // namespace tributary

int main() {
    tributary::test_write_then_read();
    tributary::test_zero_length_io();
    tributary::test_read_past_end_is_eof_not_error();
    tributary::test_open_flags();
    tributary::test_unique_fd_ownership();
    tributary::test_write_file_atomic();
    tributary::test_write_file_atomic_fails_cleanly();
    tributary::test_read_whole_file();
    tributary::test_sync();
    return tributary::ReportResults();
}
