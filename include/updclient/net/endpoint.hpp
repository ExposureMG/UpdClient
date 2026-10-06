#pragma once

#include <updclient/core/error.hpp>
#include <updclient/core/export.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace updclient::net {

// URI form: scheme://host[:port][?key=value&key=value]
// A bare "host" or "host:port" means tcp. Port 0 means "unspecified; protocol default".
// The "timeout" query option (milliseconds) is stored in Endpoint::timeout; values
// above Endpoint::kMaxTimeout are capped to it.
// Everything between "://" and "?" other than a trailing ":port" is the host, so
// non-IP schemes can carry device names (e.g. serial:///dev/ttyUSB0?baud=115200).
struct UPDCLIENT_API Endpoint {
  static constexpr std::chrono::milliseconds kMaxTimeout{std::chrono::hours(24)};

  std::string scheme;
  std::string host;
  uint16_t port = 0;
  std::map<std::string, std::string> options;
  std::chrono::milliseconds timeout{5000};

  static Result<Endpoint> parse(std::string_view uri);
  std::string toString() const;
};

} // namespace updclient::net
