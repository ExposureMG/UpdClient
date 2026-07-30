#include "network/udp_listener.hpp"
#include <spdlog/spdlog.h>
#include <cstring>
#include <utility>

namespace updclient::network {

UdpListener::~UdpListener() {
  close();
}

UdpListener::UdpListener(UdpListener &&other) noexcept {
  socketFd_ = std::exchange(other.socketFd_, INVALID_SOCKET_FD);
  boundPort_ = std::exchange(other.boundPort_, 0);
}

UdpListener &UdpListener::operator=(UdpListener &&other) noexcept {
  if (this != &other) {
    close();
    socketFd_ = std::exchange(other.socketFd_, INVALID_SOCKET_FD);
    boundPort_ = std::exchange(other.boundPort_, 0);
  }
  return *this;
}

bool UdpListener::bind(uint16_t port, bool reuseAddr) {
  close();

  socketFd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (socketFd_ == INVALID_SOCKET_FD) {
    spdlog::error("Failed to create UDP socket (err {})", get_last_socket_error());
    return false;
  }

  if (reuseAddr) {
    int reuse = 1;
    ::setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char *>(&reuse), sizeof(reuse));
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (::bind(socketFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    spdlog::error("Failed to bind UDP socket to port {} (err {})", port,
                  get_last_socket_error());
    close();
    return false;
  }

  boundPort_ = port;
  spdlog::debug("UDP socket bound to port {}", port);
  return true;
}

void UdpListener::close() {
  if (socketFd_ != INVALID_SOCKET_FD) {
    close_socket(socketFd_);
    socketFd_ = INVALID_SOCKET_FD;
  }
  boundPort_ = 0;
}

bool UdpListener::receivePacket(DatagramPacket &outPacket,
                                std::chrono::milliseconds timeoutChunk) {
  if (!isBound())
    return false;

  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(socketFd_, &readfds);

  timeval tv{};
  auto ms = timeoutChunk.count();
  tv.tv_sec = static_cast<long>(ms / 1000);
  tv.tv_usec = static_cast<long>((ms % 1000) * 1000);

  int selRet = ::select(static_cast<int>(socketFd_) + 1, &readfds, nullptr, nullptr, &tv);
  if (selRet > 0 && FD_ISSET(socketFd_, &readfds)) {
    uint8_t buffer[2048];
    sockaddr_in srcAddr{};
    socklen_t srcLen = sizeof(srcAddr);

    auto bytes = ::recvfrom(socketFd_, reinterpret_cast<char *>(buffer), sizeof(buffer), 0,
                            reinterpret_cast<sockaddr *>(&srcAddr), &srcLen);
    if (bytes > 0) {
      char ipStr[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &(srcAddr.sin_addr), ipStr, sizeof(ipStr));

      outPacket.data.assign(buffer, buffer + bytes);
      outPacket.senderIp = std::string(ipStr);
      outPacket.senderPort = ntohs(srcAddr.sin_port);
      return true;
    }
  }
  return false;
}

} // namespace updclient::network
