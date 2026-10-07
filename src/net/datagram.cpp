#include <net/datagram.hpp>

namespace updclient::net {

IDatagramSocket::~IDatagramSocket() = default;

Result<void> IDatagramSocket::bindWith(const DatagramBindOptions &options) {
  const bool plainListener = options.address.empty() && options.family == AddressFamily::IPv4 &&
                             !options.broadcast && options.multicastGroup.empty() &&
                             options.multicastInterface.empty();
  if (!plainListener) {
    return fail(ErrorCode::Unsupported,
                "this datagram socket cannot bind to an address, broadcast, join a multicast group or use IPv6");
  }
  return bind(options.port, options.reuse);
}

Result<size_t> IDatagramSocket::sendTo(std::span<const uint8_t>, std::string_view, uint16_t) {
  return fail(ErrorCode::Unsupported, "this datagram socket cannot send");
}

} // namespace updclient::net
