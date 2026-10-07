#pragma once

#include <core/export.hpp>
#include <net/datagram.hpp>

#include <memory>

namespace updclient::net {

class UPDCLIENT_API UdpSocket final : public IDatagramSocket {
public:
  UdpSocket();
  ~UdpSocket() override;

  Result<void> bind(uint16_t port, bool reuse) override;
  Result<void> bindWith(const DatagramBindOptions &options) override;
  Result<size_t> sendTo(std::span<const uint8_t> data, std::string_view address, uint16_t port) override;
  Result<std::optional<Datagram>> receive(std::chrono::milliseconds timeout) override;
  void close() noexcept override;

  bool isBound() const noexcept;
  uint16_t boundPort() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

UPDCLIENT_API std::unique_ptr<IDatagramSocket> makeUdpSocket();

} // namespace updclient::net
