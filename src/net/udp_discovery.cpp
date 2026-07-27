#include "updclient/udp_discovery.hpp"
#include "updclient/protocol.hpp"
#include "updclient/net_compat.hpp"
#include <spdlog/spdlog.h>

#include <cstring>
#include <set>

namespace updclient {

std::optional<std::string>
UdpDiscovery::discoverOne(std::chrono::milliseconds timeout) {
  auto all = discoverAll(timeout);
  if (!all.empty()) {
    return all.front().ipAddress;
  }
  return std::nullopt;
}

std::vector<DiscoveredConsole>
UdpDiscovery::discoverAll(std::chrono::milliseconds timeout) {
  std::vector<DiscoveredConsole> result;
  std::set<std::string> seenIps;

  socket_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd == INVALID_SOCKET_FD) {
    spdlog::error("Failed to create UDP socket (err {})", get_last_socket_error());
    return result;
  }

  int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ANNC_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    spdlog::error("Failed to bind UDP port {}: (err {})", ANNC_PORT, get_last_socket_error());
    close_socket(fd);
    return result;
  }

  spdlog::debug("Listening for UpdServer UDP broadcasts on port {}...",
                ANNC_PORT);

  auto startTime = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - startTime < timeout) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200'000; // 200ms polling chunks

    int selRet = ::select(static_cast<int>(fd) + 1, &readfds, nullptr, nullptr, &tv);
    if (selRet > 0 && FD_ISSET(fd, &readfds)) {
      UdpBcastMsg msg{};
      sockaddr_in srcAddr{};
      socklen_t srcLen = sizeof(srcAddr);

      auto bytes =
          ::recvfrom(fd, reinterpret_cast<char *>(&msg), sizeof(msg), 0,
                     reinterpret_cast<sockaddr *>(&srcAddr), &srcLen);
      if (bytes == sizeof(UdpBcastMsg)) {
        if (swap_be(msg.magic) == CMD_MAGIC_BE || msg.magic == CMD_MAGIC_BE) {
          char ipStr[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &(srcAddr.sin_addr), ipStr, sizeof(ipStr));
          std::string ip(ipStr);

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

  close_socket(fd);
  return result;
}

} // namespace updclient
