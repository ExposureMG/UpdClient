#pragma once

#include <updclient/core/error.hpp>
#include <updclient/core/export.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Wire-level definitions for XBDM, the Xbox 360 debug monitor: constants, status
// codes, the parsers for status lines and key=value lines, FILETIME conversion and
// the UDP name protocol packets. No I/O lives here. The protocol itself is
// described in docs/XBDM_PROTOCOL.md; section numbers below refer to it.

namespace updclient::xbdm {

inline constexpr uint16_t kXbdmPort = 730;

// Status codes (section 1.4). 2xx is success, 4xx a refusal that leaves the
// connection usable; anything else is a protocol error.
namespace status {
inline constexpr int kOk = 200;
inline constexpr int kConnected = 201;
inline constexpr int kMultiline = 202;
inline constexpr int kBinary = 203;
inline constexpr int kSendBinary = 204;
inline constexpr int kDedicated = 205;

inline constexpr int kUndefined = 400;
inline constexpr int kMaxConnections = 401;
inline constexpr int kNoSuchFile = 402;
inline constexpr int kNoModule = 403;
inline constexpr int kMemoryNotMapped = 404;
inline constexpr int kNoThread = 405;
inline constexpr int kClockNotSet = 406;
inline constexpr int kInvalidCommand = 407;
inline constexpr int kNotStopped = 408;
inline constexpr int kMustCopy = 409;
inline constexpr int kAlreadyExists = 410;
inline constexpr int kDirectoryNotEmpty = 411;
inline constexpr int kBadFileName = 412;
inline constexpr int kCannotCreate = 413;
inline constexpr int kCannotAccess = 414;
inline constexpr int kDeviceFull = 415;
inline constexpr int kNotDebuggable = 416;
inline constexpr int kInvalidArgument = 423;
inline constexpr int kAlreadyStopped = 426;
inline constexpr int kFieldNotPresent = 437;
inline constexpr int kLineTooLong = 446;
} // namespace status

// Default bounds. Every one can be changed per client through ClientOptions.
inline constexpr size_t kMaxLineBytes = 64u * 1024u;          // one received line (section 5.2.1)
inline constexpr size_t kMaxBodyBytes = 16u * 1024u * 1024u;  // all lines of one 202 body
inline constexpr size_t kMaxCommandBytes = 1024;              // one command line, CR LF included
inline constexpr uint64_t kMaxScreenshotBytes = 64u * 1024u * 1024u;
inline constexpr uint32_t kMaxMemoryReadBytes = 0x20000;      // getmem / getmemex per request
inline constexpr size_t kSetMemChunkBytes = 64;               // setmem data per command line
inline constexpr uint64_t kMaxUploadBytes = 0xFFFFFFFFull;    // sendfile beyond 4 GiB - 1 is untested
inline constexpr size_t kTransferChunkBytes = 64u * 1024u;    // host-side piece size of file transfers
inline constexpr size_t kMaxFileNameBytes = 42;               // FATX name limit, used for temporary names

// One status line: "DDD- text". The text is the fixed phrase or the result itself.
struct StatusLine {
  int code = 0;
  std::string text;

  bool isSuccess() const noexcept { return code >= 200 && code <= 299; }
  bool isRefusal() const noexcept { return code >= 400 && code <= 499; }
};

// Three decimal digits, '-', an optional space, then the text. nullopt when the
// line does not start that way. The CR has already been stripped.
UPDCLIENT_API std::optional<StatusLine> parseStatusLine(std::string_view line);

// A line of space-separated key=value, key="quoted value" and bare flags. Keys and
// flags are lower-cased (the console treats them case-insensitively); values keep
// their case. A quoted value ends at the next '"' (section 4.2); one trailing comma
// after an unquoted value is dropped (the screenshot geometry line has them).
struct Params {
  std::vector<std::pair<std::string, std::string>> values;
  std::vector<std::string> flags;
  // An unterminated quote or text glued to a closing quote. The fields that could
  // be read are still present.
  bool malformed = false;

  // The first value for key (lower case), or nullptr.
  const std::string *find(std::string_view key) const noexcept {
    for (const auto &[name, value] : values) {
      if (name == key) return &value;
    }
    return nullptr;
  }
  bool hasFlag(std::string_view flag) const noexcept {
    for (const auto &name : flags) {
      if (name == flag) return true;
    }
    return false;
  }
};

UPDCLIENT_API Params parseParams(std::string_view line);

// Decimal, or hex after 0x, 0X, 0q or 0Q, any case, leading zeros allowed. nullopt
// for anything else, and for values that do not fit the width.
UPDCLIENT_API std::optional<uint32_t> parseNumber32(std::string_view text) noexcept;
UPDCLIENT_API std::optional<uint64_t> parseNumber64(std::string_view text) noexcept;

// "0x" and lower-case hex digits without padding, the form the client sends.
UPDCLIENT_API std::string formatNumber(uint64_t value);

// FILETIME: 100 ns ticks since 1601-01-01, sent as two 32-bit halves (section 4.3).
// Whether the console means UTC or local time is not known; these conversions
// treat it as UTC. The time point uses the FILETIME tick, so no value is rounded,
// and 64-bit ticks cover every FILETIME up to the year 30828.
using FileTimeTicks = std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>;
using FileTimePoint = std::chrono::time_point<std::chrono::system_clock, FileTimeTicks>;

inline constexpr uint64_t kFileTimeUnixEpoch = 116444736000000000ull;

inline constexpr uint64_t joinHalves(uint32_t hi, uint32_t lo) noexcept {
  return (uint64_t{hi} << 32) | lo;
}

// nullopt for the few FILETIMEs past the end of the tick range.
inline std::optional<FileTimePoint> fileTimeToTimePoint(uint64_t fileTime) noexcept {
  constexpr uint64_t kMaxTicks = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
  if (fileTime >= kFileTimeUnixEpoch) {
    const uint64_t ticks = fileTime - kFileTimeUnixEpoch;
    if (ticks > kMaxTicks) return std::nullopt;
    return FileTimePoint(FileTimeTicks(static_cast<int64_t>(ticks)));
  }
  return FileTimePoint(FileTimeTicks(-static_cast<int64_t>(kFileTimeUnixEpoch - fileTime)));
}

// nullopt before 1601-01-01.
inline std::optional<uint64_t> timePointToFileTime(FileTimePoint time) noexcept {
  const int64_t ticks = time.time_since_epoch().count();
  if (ticks >= 0) return kFileTimeUnixEpoch + static_cast<uint64_t>(ticks);
  const uint64_t before = uint64_t{0} - static_cast<uint64_t>(ticks);
  if (before > kFileTimeUnixEpoch) return std::nullopt;
  return kFileTimeUnixEpoch - before;
}

// UDP name protocol on port 730 (section 2.1): one datagram each.
inline constexpr uint8_t kNameLookup = 1;   // client: 01, length, name
inline constexpr uint8_t kNameReply = 2;    // console: 02, length, name
inline constexpr uint8_t kNameWildcard = 3; // client: 03 00, every console answers

UPDCLIENT_API std::vector<uint8_t> makeWildcardQuery();
// The name must be 1..255 printable ASCII characters; otherwise InvalidArgument.
UPDCLIENT_API Result<std::vector<uint8_t>> makeNameLookup(std::string_view name);
// The name of a type-2 reply. nullopt when the type byte is wrong, the length
// byte points past the datagram, or the name is empty or not printable ASCII.
// Bytes after the announced name are ignored.
UPDCLIENT_API std::optional<std::string> parseNameReply(std::span<const uint8_t> datagram);

} // namespace updclient::xbdm
