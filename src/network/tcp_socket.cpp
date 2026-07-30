#include "network/tcp_socket.hpp"
#include <spdlog/spdlog.h>
#include <cstring>
#include <utility>

namespace updclient::network {

TcpSocket::~TcpSocket() {
  disconnect();
}

TcpSocket::TcpSocket(TcpSocket &&other) noexcept {
  socketFd_ = std::exchange(other.socketFd_, INVALID_SOCKET_FD);
  targetIp_ = std::move(other.targetIp_);
  targetPort_ = std::exchange(other.targetPort_, 0);
}

TcpSocket &TcpSocket::operator=(TcpSocket &&other) noexcept {
  if (this != &other) {
    disconnect();
    socketFd_ = std::exchange(other.socketFd_, INVALID_SOCKET_FD);
    targetIp_ = std::move(other.targetIp_);
    targetPort_ = std::exchange(other.targetPort_, 0);
  }
  return *this;
}

bool TcpSocket::connect(const std::string &ipAddress, uint16_t port, int timeoutMs) {
  disconnect();

  socketFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socketFd_ == INVALID_SOCKET_FD) {
    spdlog::error("Failed to create TCP socket (err {})", get_last_socket_error());
    return false;
  }

  sockaddr_in serverAddr{};
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(port);

  if (::inet_pton(AF_INET, ipAddress.c_str(), &serverAddr.sin_addr) <= 0) {
    spdlog::error("Invalid IP address: {}", ipAddress);
    disconnect();
    return false;
  }

  set_socket_timeout(socketFd_, timeoutMs);

  if (::connect(socketFd_, reinterpret_cast<sockaddr *>(&serverAddr),
                sizeof(serverAddr)) < 0) {
    spdlog::error("Failed to connect TCP socket to {}:{} (err {})", ipAddress,
                  port, get_last_socket_error());
    disconnect();
    return false;
  }

  targetIp_ = ipAddress;
  targetPort_ = port;
  spdlog::debug("TCP socket connected to {}:{}", ipAddress, port);
  return true;
}

void TcpSocket::disconnect() {
  if (socketFd_ != INVALID_SOCKET_FD) {
    close_socket(socketFd_);
    socketFd_ = INVALID_SOCKET_FD;
  }
  targetIp_.clear();
  targetPort_ = 0;
}

bool TcpSocket::sendRaw(const void *data, size_t length) {
  if (!isConnected())
    return false;
  size_t totalSent = 0;
  const auto *ptr = static_cast<const char *>(data);

  while (totalSent < length) {
    auto sent = ::send(socketFd_, ptr + totalSent,
                       static_cast<int>(length - totalSent), 0);
    if (sent <= 0) {
      spdlog::error("Socket send failed (err {})", get_last_socket_error());
      return false;
    }
    totalSent += sent;
  }
  return true;
}

bool TcpSocket::recvRaw(void *data, size_t length) {
  if (!isConnected())
    return false;
  size_t totalRead = 0;
  auto *ptr = static_cast<char *>(data);

  while (totalRead < length) {
    auto bytes = ::recv(socketFd_, ptr + totalRead,
                        static_cast<int>(length - totalRead), 0);
    if (bytes <= 0) {
      spdlog::error("Socket receive failed (err {})", get_last_socket_error());
      return false;
    }
    totalRead += bytes;
  }
  return true;
}

bool TcpSocket::sendString(const std::string &str) {
  return sendRaw(str.c_str(), str.length());
}

std::string TcpSocket::recvString(size_t maxLen) {
  if (!isConnected() || maxLen == 0)
    return {};
  std::vector<char> buf(maxLen + 1);
  auto bytes = ::recv(socketFd_, buf.data(), static_cast<int>(maxLen), 0);
  if (bytes > 0) {
    buf[bytes] = '\0';
    return std::string(buf.data());
  }
  return {};
}

} // namespace updclient::network
