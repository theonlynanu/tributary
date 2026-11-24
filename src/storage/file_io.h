/**
 * Essentially some wrappers around POSIX file calls.
 *
 * These were written so the rest of the storage code doesn't have to handle
 * partial reads/writes, EINTR, or closing descriptors on every single error
 * path. Every function that can fail returns false and sets errno, so callers
 * can simply report std::seterror(errno).
 */

#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/unique_fd.h"

namespace tributary {

/**
 * Opens path with open(2) and flags, retrying if interrupted.
 *
 * O_CLOEXEC is always added. mode is only used when flags includes O_CREAT.
 * Returns an invalid UniqueFd on failure, with errno set.
 */
UniqueFd OpenFile(const std::string& path, int flags, mode_t mode = 0644);

/**
 * Writes exactly n bytes from data at byte offset of fd.
 *
 * Uses pwrite, descriptors shared file position is neither used nor moved, so
 * that concurrent reads at other offsets are not affected. Retries short writes
 * and EINTR. Returns false for all other errors.
 *
 * Note that after failure, some prefix of the data may have been written.
 */
bool WriteAll(int fd, const uint8_t* data, std::size_t n, uint64_t offset);

/**
 * Reads exactly n bytes at byte offset of fd into data.
 *
 * Uses pread, meaning it is safe to call from multiple threads at once. Retries
 * short reads and EINTR. Returns false if file ends before n bytes (errno=0)
 * or on I/O errors (errno set accordingly).
 */
bool ReadAll(int fd, uint8_t* data, size_t n, uint64_t offset);

/**
 * Flushes file's data and metadata to stable storage (fsync).
 *
 * On Linux, this reaches disk. On macOS, fsync stops at the drive's own cache.
 */
bool SyncFile(int fd);

/**
 * Flushes a directory's entries to stable storage.
 *
 * Creating, renaming, or deleting a file changes its directory. That change is
 * not durable until the directory itself is synced.
 *
 * This should be called after creating a segment or after WriteFileAtomic's
 * rename.
 */
bool SyncDir(const std::string& dir);

/**
 * Replaces the contents of path with data, atomically.
 *
 * Writes 'path' + '.tmp', syncs it, renames it over path, then sysncs the
 * directory. A reader or restart sees either the complete old contents or the
 * complete new contents, never a combination. On failure, the temp file is
 * removed and path is unchanged.
 */
bool WriteFileAtomic(const std::string& path, const std::vector<uint8_t>& data);

/**
 * Reads the entire file at path into out, replacing its contents.
 *
 * Returns false if the file cannot be opened (errno=ENOENT) or read.
 */
bool ReadWholeFile(const std::string& path, std::vector<uint8_t>* out);

}  // namespace tributary
