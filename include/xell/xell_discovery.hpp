#pragma once

#include "cpp_compat.hpp"
#include <chrono>
#include <optional>
#include <string>

namespace updclient::xell {

class UPDCLIENT_API XellDiscovery {
public:
  static bool probeConsole(const std::string &ipAddress,
                          uint16_t port = 80,
                          int timeoutMs = 1000);
};

} // namespace updclient::xell
