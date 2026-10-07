#include <net/udp_socket.hpp>

#include "net/platform/socket_platform.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace updclient::net {

struct UdpSocket::Impl {
  std::optional<platform::RuntimeGuard> runtime;
  platform::Socket socket;
  uint16_t port = 0;
  int family = AF_INET;
  std::vector<uint8_t> buffer = std::vector<uint8_t>(64 * 1024);
};

UdpSocket::UdpSocket() : impl_(std::make_unique<Impl>()) {}

UdpSocket::~UdpSocket() = default;

Result<void> UdpSocket::bind(uint16_t port, bool reuse) {
  DatagramBindOptions options;
  options.port = port;
  options.reuse = reuse;
  return bindWith(options);
}

Result<void> UdpSocket::bindWith(const DatagramBindOptions &options) {
  close();

  auto runtime = platform::RuntimeGuard::acquire();
  if (!runtime) return unexpected<Error>(runtime.error());

  const int family = options.family == AddressFamily::IPv6 ? AF_INET6 : AF_INET;
  auto addresses = platform::resolve(options.address, options.port, SOCK_DGRAM, true, family);
  if (!addresses) return unexpected<Error>(addresses.error());
  const auto &address = addresses->front();

  std::optional<platform::SocketAddress> group;
  if (!options.multicastGroup.empty()) {
    auto resolved = platform::resolve(options.multicastGroup, options.port, SOCK_DGRAM, false, family);
    if (!resolved) return unexpected<Error>(resolved.error());
    group = resolved->front();
  }

  auto socket = platform::createSocket(address.storage.ss_family, SOCK_DGRAM);
  if (!socket) return unexpected<Error>(socket.error());

  if (options.reuse) {
    if (auto r = platform::setReuseAddress(*socket); !r) return r;
  }
  if (options.broadcast) {
    if (auto r = platform::setBroadcast(*socket); !r) return r;
  }
  if (auto r = platform::bindSocket(*socket, address); !r) return r;
  if (group) {
    if (auto r = platform::joinMulticast(*socket, *group, options.multicastInterface); !r) return r;
  }

  uint16_t bound = options.port;
  if (options.port == 0) {
    auto actual = platform::localPort(*socket);
    if (!actual) return unexpected<Error>(actual.error());
    bound = *actual;
  }

  impl_->runtime = std::move(*runtime);
  impl_->socket = std::move(*socket);
  impl_->port = bound;
  impl_->family = family;
  spdlog::debug("udp socket bound to port {}", bound);
  return {};
}

Result<size_t> UdpSocket::sendTo(std::span<const uint8_t> data, std::string_view address, uint16_t port) {
  if (!isBound()) return fail(ErrorCode::NotConnected, "udp socket is not bound");
  if (port == 0) return fail(ErrorCode::InvalidArgument, "datagram destination has no port");

  auto resolved = platform::resolve(address, port, SOCK_DGRAM, false, impl_->family);
  if (!resolved) return unexpected<Error>(resolved.error());
  return platform::sendTo(impl_->socket, data, resolved->front());
}

Result<std::optional<Datagram>> UdpSocket::receive(std::chrono::milliseconds timeout) {
  if (!isBound()) return fail(ErrorCode::NotConnected, "udp socket is not bound");

  auto ready = platform::waitReadable(impl_->socket, std::max(timeout, std::chrono::milliseconds(0)));
  if (!ready) return unexpected<Error>(ready.error());
  if (!*ready) return std::optional<Datagram>{};

  platform::SocketAddress from;
  auto received = platform::recvFrom(impl_->socket, impl_->buffer, from);
  if (!received) return unexpected<Error>(received.error());

  Datagram datagram;
  datagram.data.assign(impl_->buffer.begin(), impl_->buffer.begin() + static_cast<std::ptrdiff_t>(*received));
  datagram.senderAddress = platform::addressToString(from);
  datagram.senderPort = platform::addressPort(from);
  return std::optional<Datagram>(std::move(datagram));
}

void UdpSocket::close() noexcept {
  impl_->socket.close();
  impl_->runtime.reset();
  impl_->port = 0;
}

bool UdpSocket::isBound() const noexcept {
  return impl_->socket.valid();
}

uint16_t UdpSocket::boundPort() const noexcept {
  return impl_->port;
}

std::unique_ptr<IDatagramSocket> makeUdpSocket() {
  return std::make_unique<UdpSocket>();
}

} // namespace updclient::net
