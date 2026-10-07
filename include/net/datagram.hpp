#pragma once

#include <core/error.hpp>
#include <core/export.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace updclient::net {

struct Datagram {
  std::vector<uint8_t> data;
  std::string senderAddress;
  uint16_t senderPort = 0;
};

enum class AddressFamily { IPv4, IPv6 };

// Everything a datagram socket can be asked to do when it is opened. Addresses are
// numeric; hostnames are not resolved.
struct DatagramBindOptions {
  // Local address to bind. Empty binds the wildcard address of the chosen family.
  std::string address;
  // 0 lets the system pick; the chosen port is reported by UdpSocket::boundPort().
  uint16_t port = 0;
  AddressFamily family = AddressFamily::IPv4;
  bool reuse = false;
  // Permits sendTo() to broadcast addresses.
  bool broadcast = false;
  // Group to join after binding (e.g. "239.255.255.250" or "ff02::c"); empty for none.
  std::string multicastGroup;
  // Interface of the membership: a local IPv4 address, or an IPv6 interface index.
  // Empty lets the system choose.
  std::string multicastInterface;
};

class UPDCLIENT_API IDatagramSocket {
public:
  IDatagramSocket() = default;
  IDatagramSocket(const IDatagramSocket &) = delete;
  IDatagramSocket &operator=(const IDatagramSocket &) = delete;
  virtual ~IDatagramSocket();

  // Passive listener: IPv4 wildcard address, no broadcast or multicast.
  virtual Result<void> bind(uint16_t port, bool reuse) = 0;
  // General form for providers that send probes, broadcast, join a multicast group
  // or use IPv6. A socket must be opened by bind() or bindWith() before sendTo()
  // and receive(); to send only, bind port 0. The default implementation forwards
  // plain IPv4 listener options to bind() and reports ErrorCode::Unsupported for
  // anything else, so existing implementations keep working.
  virtual Result<void> bindWith(const DatagramBindOptions &options);
  // Sends one datagram to a numeric address. Returns the number of bytes sent.
  // The default implementation reports ErrorCode::Unsupported.
  virtual Result<size_t> sendTo(std::span<const uint8_t> data, std::string_view address, uint16_t port);
  // Waits up to timeout for one datagram; std::nullopt means none arrived.
  virtual Result<std::optional<Datagram>> receive(std::chrono::milliseconds timeout) = 0;
  virtual void close() noexcept = 0;
};

using DatagramSocketFactory = std::function<std::unique_ptr<IDatagramSocket>()>;

} // namespace updclient::net
