/**
 * Helpers shared by tests that touch the filesystem.
 *
 * TempDir is a self-deleting temp directory. Other functions help test file
 * damage.
 */
#pragma once

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "storage/record_batch.h"

namespace tributary {

/**
 * A fresh, empty directory that is deleted when out of scope. Used in testing,
 * each test should make its own so other tests never interact with each others'
 * files.
 */
class TempDir {
   public:
    TempDir() {
        const std::filesystem::path base =
            std::filesystem::temp_directory_path();

        // pid keeps test programs running concurrently separate, counter keeps
        // TempDir objects within the same program separate.
        for (int attempt = 0; attempt < 1000; attempt++) {
            const std::filesystem::path candidate =
                base / ("tributary-test-" + std::to_string(::getpid()) + "-" +
                        std::to_string(next_id_++));
            if (std::filesystem::create_directories(candidate)) {
                path_ = candidate.string();
                return;
            }
        }
        throw std::runtime_error("TempDir: could not create a directory");
    }

    ~TempDir() {
        std::error_code ec;  // destructor should not throw
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /**
     * Get base directory path.
     */
    const std::string& path() const { return path_; }

    /**
     * Get full path of a file in this directory.
     */
    std::string file(const std::string& name) const {
        return (std::filesystem::path(path_) / name).string();
    }

   private:
    inline static int next_id_ = 0;
    std::string path_;
};

/**
 * Get size of file from full filepath.
 */
inline uint64_t FileSize(const std::string& path) {
    return std::filesystem::file_size(path);
}

inline bool FileExists(const std::string& path) {
    return std::filesystem::exists(path);
}

/**
 * Get list of all regular files in `dir`, sorted alphabetically.
 *
 * Useful for checking which segment files a log created or deleted.
 */
inline std::vector<std::string> ListFiles(const std::string& dir) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file()) {
            names.push_back(entry.path().filename().string());
        }
    }

    std::sort(names.begin(), names.end());
    return names;
}

/**
 * Simulates a write cut off by a crash by removing the last n bytes.
 */
inline void ChopBytes(const std::string& path, uint64_t n) {
    const uint64_t size = FileSize(path);
    std::filesystem::resize_file(path, n > size ? 0 : size - n);
}

/**
 * Simulate on-disk damage by flipping the lowest bit of one byte.
 */
inline void FlipByte(const std::string& path, uint64_t position) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) throw std::runtime_error("FlipByte: cannot open " + path);

    f.seekg(static_cast<std::streamoff>(position));
    char c = 0;
    f.read(&c, 1);
    if (!f) throw std::runtime_error("FlipByte: position past end of " + path);
    c = static_cast<char>(c ^ 0x01);
    f.seekp(static_cast<std::streamoff>(position));
    f.write(&c, 1);
}

/**
 * Reads or replaces a whole file directly, bypassing the code under test.
 */
inline std::vector<uint8_t> ReadFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

inline void WriteFileBytes(const std::string& path,
                           const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

inline std::ostream& operator<<(std::ostream& os, BatchStatus s) {
    switch (s) {
        case BatchStatus::kOk:
            return os << "kOk";
        case BatchStatus::kIncomplete:
            return os << "kIncomplete";
        case BatchStatus::kCorrupt:
            return os << "kCorrupt";
    }

    return os << "BatchStatus()" << static_cast<int>(s) << ")";
}

}  // namespace tributary
