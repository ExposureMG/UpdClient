#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ut {

using Bytes = std::vector<uint8_t>;

inline Bytes bytesOf(std::string_view text) {
  return Bytes(text.begin(), text.end());
}

inline std::string textOf(std::span<const uint8_t> bytes) {
  return std::string(bytes.begin(), bytes.end());
}

inline Bytes be16(uint16_t v) {
  return {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
}

inline Bytes be32(uint32_t v) {
  return {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
          static_cast<uint8_t>(v)};
}

inline void append(Bytes &out, std::span<const uint8_t> more) {
  out.insert(out.end(), more.begin(), more.end());
}

inline void append(Bytes &out, std::string_view more) {
  out.insert(out.end(), more.begin(), more.end());
}

inline Bytes concat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const auto &part : parts) append(out, part);
  return out;
}

// Deterministic, non-repeating-looking filler so shifted or dropped data is detected.
inline Bytes patternBytes(size_t count, uint32_t seed = 1) {
  Bytes out(count);
  uint32_t state = seed * 2654435761u + 12345u;
  for (auto &b : out) {
    state = state * 1664525u + 1013904223u;
    b = static_cast<uint8_t>(state >> 24);
  }
  return out;
}

inline std::optional<Bytes> readFile(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline bool writeFile(const std::filesystem::path &path, std::span<const uint8_t> data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}

// A scratch directory removed (recursively) when the object goes out of scope.
class TempDir {
public:
  TempDir() {
    std::error_code ec;
    const auto base = std::filesystem::temp_directory_path(ec);
    std::random_device rd;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = (ec ? std::filesystem::path(".") : base) /
            ("updclient_tests_" + std::to_string(stamp) + "_" + std::to_string(rd()));
    created_ = std::filesystem::create_directories(path_, ec) && !ec;
  }
  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  bool ok() const noexcept { return created_; }
  const std::filesystem::path &path() const noexcept { return path_; }
  std::filesystem::path file(std::string_view name) const { return path_ / std::string(name); }

  // Sorted names of everything directly inside the directory.
  std::vector<std::string> entries() const {
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(path_, ec), end; !ec && it != end; it.increment(ec)) {
      names.push_back(it->path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
  }

private:
  std::filesystem::path path_;
  bool created_ = false;
};

} // namespace ut
