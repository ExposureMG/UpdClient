#pragma once

#include "cli/context.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace updclient::cli {

// Paths come from UTF-8 command-line text through pathFromUtf8 (updclient/core/path.hpp).

// Writes to "<path>.part" and renames, so a failed write never leaves a truncated file.
Outcome<void> writeFile(const std::filesystem::path &path, std::span<const uint8_t> data);

// Fails with a usage error when the file is missing or larger than maxBytes.
Outcome<std::vector<uint8_t>> readFile(const std::filesystem::path &path, uint64_t maxBytes);

// Size of an existing file, or 0 when it cannot be determined.
uint64_t fileSizeOrZero(const std::filesystem::path &path);

} // namespace updclient::cli
