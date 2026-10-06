#pragma once

#include <updclient/core/export.hpp>
#include <updclient/net/endpoint.hpp>
#include <updclient/net/transport.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace updclient::net {

// IPv4 numeric addresses today. The implementation resolves through
// getaddrinfo, so IPv6 and hostnames are a small change inside the platform layer.
class UPDCLIENT_API TcpTransport final : public ITransport {
public:
  // Endpoint::port must be non-zero. Endpoint::timeout bounds the connect and
  // becomes the initial read/write timeout.
  static Result<TransportPtr> connect(const Endpoint &endpoint);
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
