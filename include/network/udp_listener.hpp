#pragma once

#include "cpp_compat.hpp"
#include "net_compat.hpp"
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace updclient::network {

struct DatagramPacket {
  std::vector<uint8_t> data;
  std::string senderIp;
  uint16_t senderPort = 0;
};

class UPDCLIENT_API UdpListener {
public:
  UdpListener() = default;
  ~UdpListener();

  UdpListener(const UdpListener &) = delete;
  UdpListener &operator=(const UdpListener &) = delete;
  UdpListener(UdpListener &&other) noexcept;
  UdpListener &operator=(UdpListener &&other) noexcept;

  bool bind(uint16_t port, bool reuseAddr = true);
  void close();
  bool isBound() const noexcept { return socketFd_ != INVALID_SOCKET_FD; }
  uint16_t boundPort() const noexcept { return boundPort_; }

  bool receivePacket(DatagramPacket &outPacket,
                     std::chrono::milliseconds timeoutChunk = std::chrono::milliseconds(200));

private:
  socket_t socketFd_ = INVALID_SOCKET_FD;
  uint16_t boundPort_ = 0;
};

} // namespace updclient::network
