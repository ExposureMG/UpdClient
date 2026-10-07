#pragma once

#include <core/export.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace updclient {

// Text that crosses the library boundary (messages, JSON, command lines) is UTF-8.
// On Windows a std::string handed to std::filesystem is read in the ANSI code page,
// so a path that arrives as UTF-8 text must go through pathFromUtf8 first, and a
// path put into text must go through pathToUtf8. Both are identity-like on POSIX.
UPDCLIENT_API std::filesystem::path pathFromUtf8(std::string_view text);
UPDCLIENT_API std::string pathToUtf8(const std::filesystem::path &path);

} // namespace updclient
