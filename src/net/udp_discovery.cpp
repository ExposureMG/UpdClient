#include "updclient/udp_discovery.hpp"
#include "updclient/protocol.hpp"
#include <spdlog/spdlog.h>

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <set>
#include <sys/socket.h>
#include <unistd.h>

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

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    spdlog::error("Failed to create UDP socket: {}", strerror(errno));
    return result;
  }

  int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ANNC_PORT);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    if (errno == EACCES || errno == EPERM) {
      spdlog::error("Failed to bind UDP port {}: {} (Port < 1024 requires root "
                    "privileges: try 'sudo ./updclient discover')",
                    ANNC_PORT, strerror(errno));
    } else {
      spdlog::error("Failed to bind UDP port {}: {}", ANNC_PORT,
                    strerror(errno));
    }
    ::close(fd);
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

    int selRet = ::select(fd + 1, &readfds, nullptr, nullptr, &tv);
    if (selRet > 0 && FD_ISSET(fd, &readfds)) {
      UdpBcastMsg msg{};
      sockaddr_in srcAddr{};
      socklen_t srcLen = sizeof(srcAddr);

      ssize_t bytes =
          ::recvfrom(fd, &msg, sizeof(msg), 0,
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

  ::close(fd);
  return result;
}

} // namespace updclient
