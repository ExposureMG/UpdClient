#include <updclient/protocols/xbdm/path.hpp>

#include <string_view>
#include <vector>

namespace updclient::xbdm {

namespace {

constexpr size_t kMaxDriveNameBytes = 35;
constexpr std::string_view kForbiddenInNames = "\"\\/:*?<>|";

bool isAsciiAlnum(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool isUnquotable(char c) noexcept {
  const auto u = static_cast<unsigned char>(c);
  return u < 0x20 || u > 0x7E || c == '"';
}

std::string shown(std::string_view text) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E) {
      out += "\\x";
      out += digits[u >> 4];
      out += digits[u & 0xF];
    } else {
      out += c;
    }
  }
  return out;
}

unexpected<Error> badPath(std::string_view path, std::string_view why) {
  return fail(ErrorCode::InvalidArgument, "invalid console path '" + shown(path) + "': " + std::string(why));
}

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  for (;;) {
    const size_t next = text.find(separator, start);
    if (next == std::string_view::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, next - start));
    start = next + 1;
  }
}

struct Parsed {
  std::string drive;
  std::vector<std::string_view> names;
};

Result<Parsed> parseConsolePath(std::string_view path) {
  const size_t colon = path.find(':');
  if (colon == std::string_view::npos) return badPath(path, "no drive (expected DRIVE:\\...)");
  Parsed parsed;
  parsed.drive = std::string(path.substr(0, colon));
  if (auto r = validateDriveName(parsed.drive); !r) return badPath(path, r.error().message);

  std::string_view rest = path.substr(colon + 1);
  if (rest.empty()) return parsed;
  if (rest.front() != '\\') return badPath(path, "the drive must be followed by '\\'");
  rest.remove_prefix(1);
  if (rest.empty()) return parsed;
  if (rest.back() == '\\') rest.remove_suffix(1);
  if (rest.empty()) return badPath(path, "empty name");

  for (std::string_view name : split(rest, '\\')) {
    if (auto r = validateName(name); !r) return badPath(path, r.error().message);
    parsed.names.push_back(name);
  }
  return parsed;
}

std::string render(const std::string &drive, const std::vector<std::string_view> &names, size_t count) {
  std::string out = drive + ":\\";
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) out.push_back('\\');
    out.append(names[i]);
  }
  return out;
}

} // namespace

Result<void> validateName(std::string_view name) {
  if (name.empty()) return fail(ErrorCode::InvalidArgument, "empty name");
  if (name == "." || name == "..") {
    return fail(ErrorCode::InvalidArgument, "'" + std::string(name) + "' is not a valid name");
  }
  for (char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E) {
      return fail(ErrorCode::InvalidArgument,
                  "name '" + shown(name) + "' contains a control character or a byte above 0x7E");
    }
    if (kForbiddenInNames.find(c) != std::string_view::npos) {
      return fail(ErrorCode::InvalidArgument,
                  "name '" + shown(name) + "' contains '" + std::string(1, c) + "', which is not allowed");
    }
  }
  return {};
}

Result<void> validateDriveName(std::string_view drive) {
  if (drive.empty() || drive.size() > kMaxDriveNameBytes) {
    return fail(ErrorCode::InvalidArgument, "a drive name has 1 to 35 characters");
  }
  for (char c : drive) {
    if (!isAsciiAlnum(c)) {
      return fail(ErrorCode::InvalidArgument, "drive name '" + shown(drive) + "' is not letters and digits");
    }
  }
  return {};
}

Result<std::string> canonicalPath(std::string_view path) {
  auto parsed = parseConsolePath(path);
  if (!parsed) return unexpected<Error>(parsed.error());
  return render(parsed->drive, parsed->names, parsed->names.size());
}

Result<std::string> toConsolePath(std::string_view slashPath) {
  if (slashPath.empty() || slashPath.front() != '/') {
    return fail(ErrorCode::InvalidArgument, "path '" + shown(slashPath) + "' must start with '/'");
  }
  std::string_view rest = slashPath.substr(1);
  if (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);
  if (rest.empty()) {
    return fail(ErrorCode::InvalidArgument, "path '" + shown(slashPath) + "' names no drive");
  }
  const auto parts = split(rest, '/');
  if (auto r = validateDriveName(parts.front()); !r) {
    return fail(ErrorCode::InvalidArgument, "invalid path '" + shown(slashPath) + "': " + r.error().message);
  }
  std::vector<std::string_view> names(parts.begin() + 1, parts.end());
  for (std::string_view name : names) {
    if (auto r = validateName(name); !r) {
      return fail(ErrorCode::InvalidArgument, "invalid path '" + shown(slashPath) + "': " + r.error().message);
    }
  }
  return render(std::string(parts.front()), names, names.size());
}

Result<std::string> fromConsolePath(std::string_view consolePath) {
  auto parsed = parseConsolePath(consolePath);
  if (!parsed) return unexpected<Error>(parsed.error());
  std::string out = "/" + parsed->drive;
  for (std::string_view name : parsed->names) {
    out.push_back('/');
    out.append(name);
  }
  return out;
}

bool isDriveRoot(std::string_view consolePath) noexcept {
  auto parsed = parseConsolePath(consolePath);
  return parsed && parsed->names.empty();
}

Result<std::string> driveOf(std::string_view consolePath) {
  auto parsed = parseConsolePath(consolePath);
  if (!parsed) return unexpected<Error>(parsed.error());
  return parsed->drive;
}

Result<std::string> parentOf(std::string_view consolePath) {
  auto parsed = parseConsolePath(consolePath);
  if (!parsed) return unexpected<Error>(parsed.error());
  if (parsed->names.empty()) return badPath(consolePath, "a drive root has no parent");
  return render(parsed->drive, parsed->names, parsed->names.size() - 1);
}

Result<std::string> nameOf(std::string_view consolePath) {
  auto parsed = parseConsolePath(consolePath);
  if (!parsed) return unexpected<Error>(parsed.error());
  if (parsed->names.empty()) return badPath(consolePath, "a drive root has no name");
  return std::string(parsed->names.back());
}

Result<std::string> joinPath(std::string_view directory, std::string_view name) {
  auto parsed = parseConsolePath(directory);
  if (!parsed) return unexpected<Error>(parsed.error());
  if (auto r = validateName(name); !r) return unexpected<Error>(r.error());
  parsed->names.push_back(name);
  return render(parsed->drive, parsed->names, parsed->names.size());
}

Result<std::string> quoteValue(std::string_view value) {
  for (char c : value) {
    if (isUnquotable(c)) {
      return fail(ErrorCode::InvalidArgument,
                  "'" + shown(value) + "' contains '\"', a control character or a byte above 0x7E, "
                  "which cannot be sent in a quoted value");
    }
  }
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  out.append(value);
  out.push_back('"');
  return out;
}

} // namespace updclient::xbdm
