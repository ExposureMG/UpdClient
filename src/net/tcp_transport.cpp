#include <net/tcp_transport.hpp>

#include "net/deadline.hpp"
#include "net/platform/socket_platform.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

namespace updclient::net {

struct TcpTransport::Impl {
  platform::RuntimeGuard runtime;
  platform::Socket socket;
  platform::WakeSignal wake;
  std::string peer;
  std::chrono::milliseconds timeout{0};

  // close() may run on another thread while a read or write is in progress. The
  // handles are released only once no call is using them, so a descriptor is
  // never closed (and possibly reused) under a thread blocked in poll or recv.
  std::mutex mutex;
  std::atomic<bool> closed{false};
  int activeCalls = 0;

  // Keeps the handles open for the duration of one read or write.
  class Call {
  public:
    explicit Call(Impl &impl) : impl_(impl), entered_(impl.enter()) {}
    Call(const Call &) = delete;
    Call &operator=(const Call &) = delete;
    ~Call() {
      if (entered_) impl_.leave();
    }
    explicit operator bool() const noexcept { return entered_; }

  private:
    Impl &impl_;
    bool entered_;
  };

  explicit Impl(platform::RuntimeGuard guard) : runtime(std::move(guard)) {}

  bool enter() {
    std::lock_guard<std::mutex> lock(mutex);
    if (closed.load()) return false;
    ++activeCalls;
    return true;
  }

  void leave() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (--activeCalls == 0 && closed.load()) release();
  }

  void shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (closed.exchange(true)) return;
    platform::shutdownBoth(socket);
    wake.signal();
    if (activeCalls == 0) release();
  }

  void release() noexcept {
    socket.close();
    wake.close();
  }

  // The socket is non-blocking: try the operation, and wait for readiness (or for
  // close()) only when it would block.
  template <class Op>
  Result<size_t> transfer(platform::WaitFor direction, const char *what, Op op) {
    using Clock = std::chrono::steady_clock;
    const bool infinite = timeout.count() == 0;
    const auto deadline = infinite ? Clock::time_point{} : deadlineAfter(timeout);
    for (;;) {
      if (closed.load()) return cancelled(what);
      auto done = op();
      // A shut down socket reads as end of stream or fails; neither may be taken
      // for something the peer did.
      if (closed.load() && (!done || *done == 0)) return cancelled(what);
      if (done || !platform::wouldBlock(done.error().sysError)) return done;

      auto remaining = std::chrono::milliseconds(-1);
      if (!infinite) {
        remaining = std::max(std::chrono::milliseconds(0),
                             std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()));
      }
      auto ready = platform::waitFor(socket, direction, remaining, &wake);
      if (!ready) return unexpected<Error>(ready.error());
      if (*ready == platform::WaitResult::Woken) return cancelled(what);
      if (*ready == platform::WaitResult::TimedOut) {
        return fail(ErrorCode::Timeout, std::string(what) + " on " + peer + " timed out after " +
                                            std::to_string(timeout.count()) + " ms");
      }
    }
  }

  static unexpected<Error> cancelled(const char *what) {
    return fail(ErrorCode::Cancelled, std::string(what) + " cancelled: the transport was closed");
  }
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

  auto wake = platform::WakeSignal::create();
  if (!wake) return unexpected<Error>(wake.error());

  auto impl = std::make_unique<Impl>(std::move(*runtime));
  impl->wake = std::move(*wake);
  impl->timeout = std::max(timeout, std::chrono::milliseconds(0));
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
    if (auto r = platform::setBlocking(*socket, false); !r) {
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
  return !impl_->closed.load();
}

void TcpTransport::close() noexcept {
  impl_->shutdown();
}

std::string TcpTransport::describe() const {
  return "tcp://" + impl_->peer;
}

Result<void> TcpTransport::setTimeout(std::chrono::milliseconds timeout) {
  if (!isOpen()) return fail(ErrorCode::NotConnected, "transport is not open");
  if (timeout.count() < 0) return fail(ErrorCode::InvalidArgument, "negative timeout");
  impl_->timeout = timeout;
  return {};
}

Result<size_t> TcpTransport::readSome(std::span<uint8_t> buffer) {
  Impl::Call call(*impl_);
  if (!call) return fail(ErrorCode::NotConnected, "transport is not open");
  return impl_->transfer(platform::WaitFor::Readable, "recv",
                         [&] { return platform::recvSome(impl_->socket, buffer); });
}

Result<size_t> TcpTransport::writeSome(std::span<const uint8_t> data) {
  Impl::Call call(*impl_);
  if (!call) return fail(ErrorCode::NotConnected, "transport is not open");
  if (data.empty()) return size_t{0};
  return impl_->transfer(platform::WaitFor::Writable, "send",
                         [&] { return platform::sendSome(impl_->socket, data); });
}

} // namespace updclient::net
