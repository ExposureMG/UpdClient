#pragma once

#include <core/export.hpp>
#include <net/endpoint.hpp>
#include <net/transport.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

namespace updclient::net {

// IPv4 only today: the host is an IPv4 address or a host name that resolves to one
// (the lookup runs within Endpoint::timeout). The implementation resolves through
// getaddrinfo, so IPv6 is a small change inside the platform layer.
// close() from another thread shuts the socket down and wakes a blocked read or
// write (see ITransport). No transport exists while connecting, so a connect is
// cancelled through the stop_token overload instead; without a token it is bounded
// by its timeout.
class UPDCLIENT_API TcpTransport final : public ITransport {
public:
  // Endpoint::port must be non-zero. Endpoint::timeout bounds the connect and
  // becomes the initial read/write timeout; the timeout applies to each call.
  static Result<TransportPtr> connect(const Endpoint &endpoint);
  // As above; a stop request ends the connect at once with ErrorCode::Cancelled. A
  // token that is already stopped fails before anything is sent. The token is used
  // only during the call.
  static Result<TransportPtr> connect(const Endpoint &endpoint, std::stop_token stop);
  static Result<TransportPtr> connect(std::string_view host, uint16_t port,
                                      std::chrono::milliseconds timeout);

  ~TcpTransport() override;

  bool isOpen() const noexcept override;
  void close() noexcept override;
  std::string describe() const override;
  Result<void> setTimeout(std::chrono::milliseconds timeout) override;
  Result<size_t> readSome(std::span<uint8_t> buffer) override;
  Result<size_t> writeSome(std::span<const uint8_t> data) override;

private:
  struct Impl;
  explicit TcpTransport(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace updclient::net
