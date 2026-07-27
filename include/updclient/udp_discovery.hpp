#pragma once

#include "updclient/export.hpp"
#include <string>
#include <vector>
#include <optional>
#include <chrono>

namespace updclient {

struct UPDCLIENT_API DiscoveredConsole {
    std::string ipAddress;
    std::chrono::system_clock::time_point lastSeen;
};

class UPDCLIENT_API UdpDiscovery {
public:
    static std::optional<std::string> discoverOne(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
    static std::vector<DiscoveredConsole> discoverAll(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
};

} // namespace updclient
