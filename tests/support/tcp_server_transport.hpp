#pragma once

// The server end of an accepted loopback TCP connection, as an ITransport, for the
// mock servers' listenTcp().

#include "support/loopback_server.hpp"

#include <core/error.hpp>
#include <net/transport.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <utility>

namespace ut {

// The server end of an accepted TCP connection. close() shuts the socket down,
// which wakes a recv or send blocked on another thread; the descriptor is released
// only by the destructor, after the serving thread has been joined.
class TcpServerTransport final : public updclient::net::ITransport {
public:
  explicit TcpServerTransport(SocketHandle socket) : socket_(std::move(socket)) {}

  bool isOpen() const noexcept override { return !closed_.load(); }
  void close() noexcept override {
    if (closed_.exchange(true)) return;
#if defined(_WIN32)
    ::shutdown(socket_.get(), SD_BOTH);
#else
    ::shutdown(socket_.get(), SHUT_RDWR);
#endif
  }
  std::string describe() const override { return "tcp-server://127.0.0.1"; }
  updclient::Result<void> setTimeout(std::chrono::milliseconds timeout) override {
    timeoutMs_ = timeout.count();
    return {};
  }
  updclient::Result<size_t> readSome(std::span<uint8_t> buffer) override {
    if (closed_) return updclient::fail(updclient::ErrorCode::NotConnected, "closed");
    if (buffer.empty()) return size_t{0};
    const auto ms = timeoutMs_.load();
    if (ms > 0 && !waitReadableMs(socket_.get(), static_cast<int>(ms))) {
      if (closed_) return updclient::fail(updclient::ErrorCode::Cancelled, "closed");
      return updclient::fail(updclient::ErrorCode::Timeout, "read timed out");
    }
    const int n = ::recv(socket_.get(), reinterpret_cast<char *>(buffer.data()),
                         static_cast<int>(std::min<size_t>(buffer.size(), 1 << 20)), 0);
    if (closed_) return updclient::fail(updclient::ErrorCode::Cancelled, "closed");
    if (n < 0) return updclient::fail(updclient::ErrorCode::Disconnected, "recv failed", nativeError());
    return static_cast<size_t>(n);
  }
  updclient::Result<size_t> writeSome(std::span<const uint8_t> data) override {
    if (closed_) return updclient::fail(updclient::ErrorCode::NotConnected, "closed");
    if (data.empty()) return size_t{0};
#if defined(MSG_NOSIGNAL)
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    const int n = ::send(socket_.get(), reinterpret_cast<const char *>(data.data()),
                         static_cast<int>(std::min<size_t>(data.size(), 1 << 20)), kFlags);
    if (closed_) return updclient::fail(updclient::ErrorCode::Cancelled, "closed");
    if (n <= 0) return updclient::fail(updclient::ErrorCode::Disconnected, "send failed", nativeError());
    return static_cast<size_t>(n);
  }

private:
  SocketHandle socket_;
  std::atomic<bool> closed_{false};
  std::atomic<long long> timeoutMs_{0};
};

} // namespace ut
