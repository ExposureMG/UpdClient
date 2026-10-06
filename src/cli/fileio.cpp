#include "cli/fileio.hpp"

#include <updclient/core/path.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>

namespace updclient::cli {

Outcome<void> writeFile(const std::filesystem::path &path, std::span<const uint8_t> data) {
  std::filesystem::path temporary = path;
  temporary += ".part";
  const std::string temporaryName = pathToUtf8(temporary);
  const std::string pathName = pathToUtf8(path);
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return failWith(kExitRuntime, "Io", "cannot open '" + temporaryName + "' for writing");
    file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    file.flush();
    if (!file) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return failWith(kExitRuntime, "Io", "failed writing '" + temporaryName + "'");
    }
  }
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return failWith(kExitRuntime, "Io", "cannot move '" + temporaryName + "' to '" + pathName + "': " + ec.message());
  }
  return {};
}

Outcome<std::vector<uint8_t>> readFile(const std::filesystem::path &path, uint64_t maxBytes) {
  const std::string pathName = pathToUtf8(path);
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return usageError("cannot read '" + pathName + "': " + ec.message());
  if (size > maxBytes) {
    return usageError("'" + pathName + "' is " + std::to_string(size) + " bytes; the limit is " +
                      std::to_string(maxBytes));
  }

  std::ifstream file(path, std::ios::binary);
  if (!file) return usageError("cannot open '" + pathName + "' for reading");
  std::vector<uint8_t> data(static_cast<size_t>(size));
  file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
  if (static_cast<uint64_t>(file.gcount()) != size) {
    return failWith(kExitRuntime, "Io", "short read from '" + pathName + "'");
  }
  return data;
}

uint64_t fileSizeOrZero(const std::filesystem::path &path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  return ec ? 0 : static_cast<uint64_t>(size);
}

} // namespace updclient::cli
