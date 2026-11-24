/**
 * UniqueFd owns a POSIX file descriptor and closes it when destroyed.
 */
#pragma once

namespace tributary {

/**
 * Owns a POSIX file descriptor and closes it when destroyed.
 *
 * Movable, not copyable. 1:1 relationship between a file descriptor and a
 * UniqueFd object. Default constructed UniqueFd holds nothing.
 */
class UniqueFd {
   public:
    UniqueFd() = default;

    /**
     * Takes ownership of fd. Pass -1 for "no descriptor".
     */
    explicit UniqueFd(int fd) : fd_(fd) {};

    /**
     * Closes the descriptor if one is held.
     */
    ~UniqueFd() { reset(); }

    /**
     * Takes the descriptor from other, which will be left holding nothing.
     */
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

    /**
     * Closes this object's descriptor, then takes the one from other.
     */
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    /**
     * Get the raw descriptor, or -1 if none is held.
     */
    int get() const { return fd_; }

    /**
     * Returns true if holding a descriptor.
     */
    bool valid() const { return fd_ >= 0; }

    /**
     * Closes the current descriptor if holding, then takes ownership of fd.
     */
    void reset(int fd = -1);

    /**
     * Gives up ownership without closing, and returns the descriptor.
     *
     * Caller is now responsible for closing the descriptor.
     */
    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

   private:
    int fd_ = -1;
};

}  // namespace tributary
