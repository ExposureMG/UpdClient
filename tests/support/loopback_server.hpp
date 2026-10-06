#pragma once

// A throwaway TCP server on 127.0.0.1 for the few tests that exercise the real
// socket transport. It has its own small socket layer on purpose: the library's
// private platform layer is not visible to tests, and the tests must not depend
// on the code they check.

#include "support/test_util.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace ut {

#if defined(_WIN32)
using NativeSocket = SOCKET;
inline constexpr NativeSocket kNoSocket = INVALID_SOCKET;
inline void closeNative(NativeSocket s) { ::closesocket(s); }
inline int nativeError() { return ::WSAGetLastError(); }
inline bool netStartup() {
  static const bool ok = [] {
    WSADATA data;
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ok;
}
#else
using NativeSocket = int;
inline constexpr NativeSocket kNoSocket = -1;
inline void closeNative(NativeSocket s) { ::close(s); }
inline int nativeError() { return errno; }
inline bool netStartup() { return true; }
#endif

// Waits until the socket is readable (or has a pending connection).
inline bool waitReadableMs(NativeSocket s, int timeoutMs) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  return ::select(static_cast<int>(s) + 1, &set, nullptr, nullptr, &tv) > 0;
}

inline sockaddr_in loopbackAddress(uint16_t port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return address;
}

class SocketHandle {
public:
  SocketHandle() = default;
  explicit SocketHandle(NativeSocket s) : s_(s) {}
  SocketHandle(SocketHandle &&other) noexcept : s_(other.s_) { other.s_ = kNoSocket; }
  SocketHandle &operator=(SocketHandle &&other) noexcept {
    if (this != &other) {
      reset();
      s_ = other.s_;
      other.s_ = kNoSocket;
    }
    return *this;
  }
  SocketHandle(const SocketHandle &) = delete;
  SocketHandle &operator=(const SocketHandle &) = delete;
  ~SocketHandle() { reset(); }

  bool valid() const noexcept { return s_ != kNoSocket; }
  NativeSocket get() const noexcept { return s_; }
  void reset() noexcept {
    if (s_ != kNoSocket) closeNative(s_);
    s_ = kNoSocket;
  }

private:
  NativeSocket s_ = kNoSocket;
};

// One accepted connection, as seen by the server-side handler.
class ServerConnection {
public:
  explicit ServerConnection(SocketHandle socket) : socket_(std::move(socket)) {}

  bool sendAll(const Bytes &data) {
    size_t sent = 0;
    while (sent < data.size()) {
      const int n = ::send(socket_.get(), reinterpret_cast<const char *>(data.data()) + sent,
                           static_cast<int>(std::min<size_t>(data.size() - sent, 1 << 20)), 0);
      if (n <= 0) return false;
      sent += static_cast<size_t>(n);
    }
    return true;
  }

  // Reads exactly count bytes; false on EOF, error or timeout.
  bool recvExact(Bytes &out, size_t count, int timeoutMs = 5000) {
    out.resize(count);
    size_t got = 0;
    while (got < count) {
      if (!waitReadableMs(socket_.get(), timeoutMs)) return false;
      const int n = ::recv(socket_.get(), reinterpret_cast<char *>(out.data()) + got,
                           static_cast<int>(count - got), 0);
      if (n <= 0) return false;
      got += static_cast<size_t>(n);
    }
    return true;
  }

  void close() { socket_.reset(); }

private:
  SocketHandle socket_;
};

class LoopbackServer {
public:
  using Handler = std::function<void(ServerConnection &, const std::atomic<bool> &stop)>;

  // Returns nullptr (and a reason) when the environment does not allow sockets.
  static std::unique_ptr<LoopbackServer> start(Handler handler, std::string *whyNot = nullptr) {
    auto fail = [&](const std::string &reason) -> std::unique_ptr<LoopbackServer> {
      if (whyNot) *whyNot = reason;
      return nullptr;
    };
    if (!netStartup()) return fail("socket runtime unavailable");

    SocketHandle listener(::socket(AF_INET, SOCK_STREAM, 0));
    if (!listener.valid()) return fail("socket() failed, error " + std::to_string(nativeError()));
    sockaddr_in address = loopbackAddress(0);
    if (::bind(listener.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
      return fail("bind() failed, error " + std::to_string(nativeError()));
    }
    if (::listen(listener.get(), 4) != 0) return fail("listen() failed, error " + std::to_string(nativeError()));

    sockaddr_in bound{};
#if defined(_WIN32)
    int length = sizeof(bound);
#else
    socklen_t length = sizeof(bound);
#endif
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&bound), &length) != 0) {
      return fail("getsockname() failed, error " + std::to_string(nativeError()));
    }

    std::unique_ptr<LoopbackServer> server(new LoopbackServer());
    server->port_ = ntohs(bound.sin_port);
    server->listener_ = std::move(listener);
    server->handler_ = std::move(handler);
    server->thread_ = std::thread([raw = server.get()] { raw->run(); });
    return server;
  }

  LoopbackServer(const LoopbackServer &) = delete;
  LoopbackServer &operator=(const LoopbackServer &) = delete;

  ~LoopbackServer() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  uint16_t port() const noexcept { return port_; }
  bool handledConnection() const noexcept { return handled_; }

private:
  LoopbackServer() = default;

  void run() {
    // Serve a single connection; give up quietly if nobody connects.
    for (int waited = 0; !stop_ && waited < 100; ++waited) {
      if (!waitReadableMs(listener_.get(), 100)) continue;
      NativeSocket accepted = ::accept(listener_.get(), nullptr, nullptr);
      if (accepted == kNoSocket) return;
      ServerConnection connection{SocketHandle(accepted)};
      handled_ = true;
      handler_(connection, stop_);
      return;
    }
  }

  uint16_t port_ = 0;
  SocketHandle listener_;
  Handler handler_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> handled_{false};
  std::thread thread_;
};

// Returns a port on 127.0.0.1 where nothing listens (bound once, then released).
inline uint16_t unusedLoopbackPort() {
  if (!netStartup()) return 0;
  SocketHandle probe(::socket(AF_INET, SOCK_STREAM, 0));
  if (!probe.valid()) return 0;
  sockaddr_in address = loopbackAddress(0);
  if (::bind(probe.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) return 0;
#if defined(_WIN32)
  int length = sizeof(address);
#else
  socklen_t length = sizeof(address);
#endif
  if (::getsockname(probe.get(), reinterpret_cast<sockaddr *>(&address), &length) != 0) return 0;
  return ntohs(address.sin_port);
}

} // namespace ut
