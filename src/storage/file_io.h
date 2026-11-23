#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

bool WriteAll(int fd, const uint8_t* p, std::size_t n, uint64_t offset);

bool ReadAll(int fd, uint8_t* p, size_t n, uint64_t offset);

bool SyncFile(int fd);

bool SyncDir(const std::string& dir);

bool WriteFileAtomic(const std::string& path, const std::vector<uint8_t>& data);
