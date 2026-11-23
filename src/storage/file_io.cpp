#include "file_io.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>

namespace tributary {

void UniqueFd::reset(int fd) {
    if (fd_ >= 0) {
        // close can report EINTR, but the descriptor is released
        // regardless. Retrying could close an unrelated descriptor.
        ::close(fd_);
    }
    fd_ = fd;
}

UniqueFd OpenFile(const std::string& path, int flags, mode_t mode) {
    int fd;
    do {
        fd = ::open(path.c_str(), flags | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EINTR);
    return UniqueFd(fd);
}

bool WriteAll(int fd, const uint8_t* data, std::size_t n, uint64_t offset) {
    size_t done = 0;
    while (done < n) {
        const ssize_t w = ::pwrite(fd, data + done, n - done,
                                   static_cast<off_t>(offset + done));

        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }

        // Shouldn't happen for a regular file
        if (w == 0) {
            errno = EIO;
            return false;
        }
        done += static_cast<size_t>(w);
    }

    return true;
}

bool ReadAll(int fd, uint8_t* data, size_t n, uint64_t offset) {
    size_t done = 0;
    while (done < n) {
        const ssize_t r = ::pread(fd, data + done, n - done,
                                  static_cast<off_t>(offset + done));

        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }

        if (r == 0) {
            errno = 0;  // EOF, not IO
            return false;
        }

        done += static_cast<size_t>(r);
    }
    return true;
}

bool SyncFile(int fd) { return ::fsync(fd) == 0; }

bool SyncDir(const std::string& dir) {
    UniqueFd fd = OpenFile(dir, O_RDONLY | O_DIRECTORY);
    if (!fd.valid()) return false;
    return ::fsync(fd.get()) == 0;
}

bool WriteFileAtomic(const std::string& path,
                     const std::vector<uint8_t>& data) {
    const std::string tmp = path + ".tmp";

    // Write and sync new contents under temp name
    {
        UniqueFd fd = OpenFile(tmp, O_WRONLY | O_CREAT | O_TRUNC);
        if (!fd.valid()) return false;

        if (!WriteAll(fd.get(), data.data(), data.size(), 0) ||
            !SyncFile(fd.get())) {
            const int saved = errno;
            ::unlink(tmp.c_str());
            errno = saved;
            return false;
        }
    }  // fd closed here, before rename

    // Swap into place in one step using rename()
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const int saved = errno;
        ::unlink(tmp.c_str());
        errno = saved;
        return false;
    }

    // Make rename durable
    std::string dir = std::filesystem::path(path).parent_path().string();
    if (dir.empty()) dir = ".";
    return SyncDir(dir);
}

bool ReadWholeFile(const std::string& path, std::vector<uint8_t>* out) {
    UniqueFd fd = OpenFile(path, O_RDONLY);
    if (!fd.valid()) return false;

    struct stat st;
    if (::fstat(fd.get(), &st) != 0) return false;

    out->resize(static_cast<size_t>(st.st_size));
    return ReadAll(fd.get(), out->data(), out->size(), 0);
}

}  // namespace tributary
