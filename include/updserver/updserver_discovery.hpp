#pragma once

#include "cpp_compat.hpp"
#include "updserver/protocol.hpp"
#include "network/udp_listener.hpp"
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace updclient::updserver {

struct DiscoveredConsole {
  std::string ipAddress;
  std::chrono::system_clock::time_point lastSeen;
};

class UPDCLIENT_API UpdServerDiscovery {
public:
  static std::optional<std::string>
  discoverOne(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

  static std::vector<DiscoveredConsole>
  discoverAll(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
};

} // namespace updclient::updserver
