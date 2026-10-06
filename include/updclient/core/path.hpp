#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace updclient {

// Text that crosses the library boundary (messages, JSON, command lines) is UTF-8.
// On Windows a std::string handed to std::filesystem is read in the ANSI code page,
// so a path that arrives as UTF-8 text must go through pathFromUtf8 first, and a
// path put into text must go through pathToUtf8. Both are identity-like on POSIX.
inline std::filesystem::path pathFromUtf8(std::string_view text) {
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}

inline std::string pathToUtf8(const std::filesystem::path &path) {
  const std::u8string text = path.u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

} // namespace updclient
