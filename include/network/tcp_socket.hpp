#pragma once

#include "cpp_compat.hpp"
#include "net_compat.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace updclient::network {

class UPDCLIENT_API TcpSocket {
public:
  TcpSocket() = default;
  ~TcpSocket();

  TcpSocket(const TcpSocket &) = delete;
  TcpSocket &operator=(const TcpSocket &) = delete;
  TcpSocket(TcpSocket &&other) noexcept;
  TcpSocket &operator=(TcpSocket &&other) noexcept;

  bool connect(const std::string &ipAddress, uint16_t port, int timeoutMs = 5000);
  void disconnect();
  bool isConnected() const noexcept { return socketFd_ != INVALID_SOCKET_FD; }
  const std::string &targetIp() const noexcept { return targetIp_; }
  uint16_t targetPort() const noexcept { return targetPort_; }

  bool sendRaw(const void *data, size_t length);
  bool recvRaw(void *data, size_t length);
  bool sendString(const std::string &str);
  std::string recvString(size_t maxLen = 1024);

private:
  socket_t socketFd_ = INVALID_SOCKET_FD;
  std::string targetIp_;
  uint16_t targetPort_ = 0;
};

} // namespace updclient::network
