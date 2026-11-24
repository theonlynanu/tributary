#include "unique_fd.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <string>

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

}  // namespace tributary
