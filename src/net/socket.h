/**
 * A connected TCP socket.
 *
 * Thread safety: one thread may send while another receives on the same socket
 * (takes advantage of TCP being full duplex), but two threads can never send or
 * receive at once. Shutdown() may be called from any thread to wake one blocked
 * in RecvAll.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "common/unique_fd.h"
namespace tributary {

/**
 * Result of a socket operation.
 */
enum class IoStatus {
    kOk,       // every byte sent/received
    kClosed,   // peer closed or reset connection, or Shutdown() was called
    kTimeout,  // timeout expired
    kError     // anything else (errno should be set)
};

class Socket {
   public:
    /**
     * Empty socket. valid() is false.
     */
    Socket() = default;

    /**
     * Takes ownership of a connected socket fd (from accept or connect)
     */
    explicit Socket(int fd);

    Socket(Socket&&) noexcept = default;             // Allow move construction
    Socket& operator=(Socket&&) noexcept = default;  // Allow move
    Socket(const Socket&) = delete;                  // Forbid copy construction
    Socket& operator=(const Socket&) = delete;       // Forbid copy assignment

    /**
     * Whether this socket holds a connection.
     */
    bool valid() const { return fd_.valid(); }

    /**
     * Gets the raw descriptor for logging and tests. This socket retains
     * ownership.
     */
    int fd() const { return fd_.get(); }

    /**
     * Sends exactly n bytes, retrying on partial sends and EINTR.
     *
     * Returns kOk once every byte has been handed to the kernel.
     */
    IoStatus SendAll(const uint8_t* data, size_t n);

    /**
     * Receives exactly n bytes into data, retrying on partial sends and EINTR.
     *
     * Returns kClosed if peer closes connection first, kTimeout if receive
     * timeout expires, and kOk if successfully read all n bytes.
     */
    IoStatus RecvAll(uint8_t* data, size_t n);

    /**
     * Makes each blocking recieve give up after 'timeout' milliseconds.
     *
     * 0 means wait forever (default).
     */
    bool SetRecvTimeout(std::chrono::milliseconds timeout);

    /**
     * Makes each blocking send give up after 'timeout' milliseconds.
     *
     * 0 means wait forever (default).
     */
    bool SetSendTimeout(std::chrono::milliseconds timeout);

    /**
     * Sets TCP_NODELAY. On by default with Nagle's algorithm.
     */
    bool SetNoDelay(bool on);

    /**
     * Ends connection in both directions without closing the file descriptor.
     *
     * Safe to call fro manother thread, owner must still call Close() or let
     * the socket be destroyed afterwards.
     */
    void Shutdown();

    /**
     * Closes the connection now.
     *
     * Must not be called while another thread is using the socket. Use
     * Shutdown() first to wake other threads, then call this.
     */
    void Close() { fd_.reset(); }

   private:
    UniqueFd fd_;
};

}  // namespace tributary
