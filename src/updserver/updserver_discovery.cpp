#include "updserver/updserver_discovery.hpp"
#include "updserver/protocol.hpp"
#include <spdlog/spdlog.h>

#include <cstring>
#include <set>

namespace updclient::updserver {

std::optional<std::string>
UpdServerDiscovery::discoverOne(std::chrono::milliseconds timeout) {
  auto all = discoverAll(timeout);
  if (!all.empty()) {
    return all.front().ipAddress;
  }
  return std::nullopt;
}

std::vector<DiscoveredConsole>
UpdServerDiscovery::discoverAll(std::chrono::milliseconds timeout) {
  std::vector<DiscoveredConsole> result;
  std::set<std::string> seenIps;

  network::UdpListener listener;
  if (!listener.bind(ANNC_PORT, true)) {
    spdlog::error("Failed to bind UpdServer UDP discovery on port {}", ANNC_PORT);
    return result;
  }

  spdlog::debug("Listening for UpdServer UDP broadcasts on port {}...", ANNC_PORT);

  auto startTime = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - startTime < timeout) {
    network::DatagramPacket packet;
    if (listener.receivePacket(packet, std::chrono::milliseconds(200))) {
      if (packet.data.size() == sizeof(UdpBcastMsg)) {
        UdpBcastMsg msg{};
        std::memcpy(&msg, packet.data.data(), sizeof(msg));

        if (swap_be(msg.magic) == CMD_MAGIC_BE || msg.magic == CMD_MAGIC_BE) {
          const std::string &ip = packet.senderIp;
          if (seenIps.find(ip) == seenIps.end()) {
            seenIps.insert(ip);
            result.push_back(DiscoveredConsole{
                .ipAddress = ip, .lastSeen = std::chrono::system_clock::now()});
            spdlog::info("Discovered UpdServer console at {}", ip);
          }
        }
      }
    }
  }

  return result;
}

} // namespace updclient::updserver
