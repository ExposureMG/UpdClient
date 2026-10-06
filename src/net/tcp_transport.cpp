#include <updclient/net/tcp_transport.hpp>

#include "net/platform/socket_platform.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace updclient::net {

struct TcpTransport::Impl {
  platform::RuntimeGuard runtime;
  platform::Socket socket;
  std::string peer;

  explicit Impl(platform::RuntimeGuard guard) : runtime(std::move(guard)) {}
};

TcpTransport::TcpTransport(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

TcpTransport::~TcpTransport() = default;

Result<TransportPtr> TcpTransport::connect(const Endpoint &endpoint) {
  return connect(endpoint.host, endpoint.port, endpoint.timeout);
}

Result<TransportPtr> TcpTransport::connect(std::string_view host, uint16_t port,
                                           std::chrono::milliseconds timeout) {
  if (port == 0) {
    return fail(ErrorCode::InvalidArgument, "tcp endpoint '" + std::string(host) + "' has no port");
  }

  auto runtime = platform::RuntimeGuard::acquire();
  if (!runtime) return unexpected<Error>(runtime.error());

  auto addresses = platform::resolve(host, port, SOCK_STREAM, false);
  if (!addresses) return unexpected<Error>(addresses.error());

  auto impl = std::make_unique<Impl>(std::move(*runtime));
  Error lastFailure = makeError(ErrorCode::ConnectFailed, "no address to connect to");

  for (const auto &address : *addresses) {
    auto socket = platform::createSocket(address.storage.ss_family, SOCK_STREAM);
    if (!socket) {
      lastFailure = socket.error();
      continue;
    }
    if (auto r = platform::connectWithTimeout(*socket, address, timeout); !r) {
      lastFailure = r.error();
      continue;
    }
    if (auto r = platform::setIoTimeouts(*socket, timeout); !r) {
      lastFailure = r.error();
      continue;
    }
    (void)platform::setNoDelay(*socket);
    impl->socket = std::move(*socket);
    impl->peer = platform::addressToString(address) + ":" + std::to_string(port);
    break;
  }

  if (!impl->socket.valid()) {
    return unexpected<Error>(std::move(lastFailure));
  }

  spdlog::debug("tcp connected to {}", impl->peer);
  return TransportPtr(new TcpTransport(std::move(impl)));
}

bool TcpTransport::isOpen() const noexcept {
  return impl_->socket.valid();
}

void TcpTransport::close() noexcept {
  impl_->socket.close();
}

std::string TcpTransport::describe() const {
  return "tcp://" + impl_->peer;
}

Result<void> TcpTransport::setTimeout(std::chrono::milliseconds timeout) {
  if (!isOpen()) return fail(ErrorCode::NotConnected, "transport is not open");
  if (timeout.count() < 0) return fail(ErrorCode::InvalidArgument, "negative timeout");
  return platform::setIoTimeouts(impl_->socket, timeout);
}

Result<size_t> TcpTransport::readSome(std::span<uint8_t> buffer) {
  if (!isOpen()) return fail(ErrorCode::NotConnected, "transport is not open");
  return platform::recvSome(impl_->socket, buffer);
}

Result<size_t> TcpTransport::writeSome(std::span<const uint8_t> data) {
  if (!isOpen()) return fail(ErrorCode::NotConnected, "transport is not open");
  return platform::sendSome(impl_->socket, data);
}

} // namespace updclient::net
