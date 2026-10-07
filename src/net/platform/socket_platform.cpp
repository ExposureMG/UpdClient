#include "net/platform/socket_platform.hpp"

#include "net/deadline.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstring>
#include <mutex>
#include <system_error>
#include <utility>

#if !defined(UPDCLIENT_PLATFORM_WINSOCK)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace updclient::net::platform {

namespace {

constexpr size_t kMaxIoChunk = size_t{1} << 30;

std::mutex gRuntimeMutex;
int gRuntimeRefs = 0;

bool isInterrupted(int err) noexcept {
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  return err == WSAEINTR;
#else
  return err == EINTR;
#endif
}

bool isConnectInProgress(int err) noexcept {
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY;
#else
  return err == EINPROGRESS || err == EALREADY;
#endif
}

bool isTimeoutError(int err) noexcept {
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
#else
  return err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT;
#endif
}

bool isDisconnectError(int err) noexcept {
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  return err == WSAECONNRESET || err == WSAECONNABORTED || err == WSAENOTCONN ||
         err == WSAESHUTDOWN || err == WSAENETRESET;
#else
  return err == ECONNRESET || err == EPIPE || err == ECONNABORTED || err == ENOTCONN ||
         err == ESHUTDOWN;
#endif
}

bool isConnectTimeoutError(int err) noexcept {
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  return err == WSAETIMEDOUT;
#else
  return err == ETIMEDOUT;
#endif
}

#if defined(UPDCLIENT_PLATFORM_WINSOCK) && !defined(IPV6_JOIN_GROUP) && defined(IPV6_ADD_MEMBERSHIP)
#define IPV6_JOIN_GROUP IPV6_ADD_MEMBERSHIP
#endif

#if defined(UPDCLIENT_PLATFORM_WINSOCK)
constexpr int kMsgFlags = 0;
#elif defined(MSG_NOSIGNAL)
constexpr int kMsgFlags = MSG_NOSIGNAL;
#else
constexpr int kMsgFlags = 0;
#endif

Result<WaitResult> waitSocket(SocketHandle handle, WaitFor what, std::chrono::milliseconds timeout,
                              const WakeSignal *wake) {
  using Clock = std::chrono::steady_clock;
  const bool infinite = timeout.count() < 0;
  const auto deadline = infinite ? Clock::time_point{} : deadlineAfter(timeout);
  const bool watchWake = wake != nullptr && wake->valid();

  for (;;) {
    long long remainingMs = -1;
    if (!infinite) {
      remainingMs = std::max<long long>(
          0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())
                 .count());
    }

#if defined(UPDCLIENT_PLATFORM_WINSOCK)
    fd_set readSet;
    fd_set writeSet;
    fd_set errorSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    if (what == WaitFor::Readable) {
      FD_SET(handle, &readSet);
    } else {
      FD_SET(handle, &writeSet);
      FD_SET(handle, &errorSet);
    }
    if (watchWake) FD_SET(wake->waitHandle(), &readSet);
    timeval tv{};
    timeval *ptv = nullptr;
    if (!infinite) {
      tv.tv_sec = static_cast<long>(remainingMs / 1000);
      tv.tv_usec = static_cast<long>((remainingMs % 1000) * 1000);
      ptv = &tv;
    }
    const bool useRead = what == WaitFor::Readable || watchWake;
    int rc = ::select(0, useRead ? &readSet : nullptr, what == WaitFor::Writable ? &writeSet : nullptr,
                      what == WaitFor::Writable ? &errorSet : nullptr, ptv);
    if (rc == SOCKET_ERROR) {
      int err = lastError();
      if (isInterrupted(err)) continue;
      return fail(ErrorCode::Io, "select failed: " + errorToString(err), err);
    }
    if (watchWake && FD_ISSET(wake->waitHandle(), &readSet)) return WaitResult::Woken;
    return rc > 0 ? WaitResult::Ready : WaitResult::TimedOut;
#else
    pollfd pfd[2]{};
    pfd[0].fd = handle;
    pfd[0].events = what == WaitFor::Readable ? POLLIN : POLLOUT;
    pfd[1].fd = watchWake ? wake->waitHandle() : -1;
    pfd[1].events = POLLIN;
    int timeoutArg = infinite ? -1 : static_cast<int>(std::min<long long>(remainingMs, INT_MAX));
    int rc = ::poll(pfd, watchWake ? 2 : 1, timeoutArg);
    if (rc < 0) {
      int err = lastError();
      if (isInterrupted(err)) continue;
      return fail(ErrorCode::Io, "poll failed: " + errorToString(err), err);
    }
    if (watchWake && pfd[1].revents != 0) return WaitResult::Woken;
    return rc > 0 ? WaitResult::Ready : WaitResult::TimedOut;
#endif
  }
}

Result<bool> waitSocket(SocketHandle handle, WaitFor what, std::chrono::milliseconds timeout) {
  auto r = waitSocket(handle, what, timeout, nullptr);
  if (!r) return unexpected<Error>(r.error());
  return *r == WaitResult::Ready;
}

#if !defined(UPDCLIENT_PLATFORM_WINSOCK) && !defined(__linux__)
bool makePipeEnd(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
         ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}
#endif

} // namespace

RuntimeGuard::RuntimeGuard(RuntimeGuard &&other) noexcept
    : active_(std::exchange(other.active_, false)) {}

RuntimeGuard &RuntimeGuard::operator=(RuntimeGuard &&other) noexcept {
  if (this != &other) {
    release();
    active_ = std::exchange(other.active_, false);
  }
  return *this;
}

RuntimeGuard::~RuntimeGuard() {
  release();
}

Result<RuntimeGuard> RuntimeGuard::acquire() {
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  std::lock_guard<std::mutex> lock(gRuntimeMutex);
  if (gRuntimeRefs == 0) {
    WSADATA data;
    int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
      return fail(ErrorCode::Io, "WSAStartup failed: " + errorToString(rc), rc);
    }
  }
  ++gRuntimeRefs;
  return RuntimeGuard(true);
#else
  (void)gRuntimeMutex;
  (void)gRuntimeRefs;
  return RuntimeGuard(false);
#endif
}

void RuntimeGuard::release() noexcept {
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  if (!active_) return;
  active_ = false;
  std::lock_guard<std::mutex> lock(gRuntimeMutex);
  if (--gRuntimeRefs == 0) {
    ::WSACleanup();
  }
#endif
}

Socket::Socket(Socket &&other) noexcept
    : handle_(std::exchange(other.handle_, kInvalidSocket)) {}

Socket &Socket::operator=(Socket &&other) noexcept {
  if (this != &other) {
    close();
    handle_ = std::exchange(other.handle_, kInvalidSocket);
  }
  return *this;
}

Socket::~Socket() {
  close();
}

void Socket::close() noexcept {
  if (handle_ == kInvalidSocket) return;
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  ::closesocket(handle_);
#else
  ::close(handle_);
#endif
  handle_ = kInvalidSocket;
}

Result<WakeSignal> WakeSignal::create() {
  WakeSignal wake;
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  auto socket = createSocket(AF_INET, SOCK_DGRAM);
  if (!socket) return unexpected<Error>(socket.error());
  SocketAddress address;
  sockaddr_in loopback{};
  loopback.sin_family = AF_INET;
  loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  std::memcpy(&address.storage, &loopback, sizeof(loopback));
  address.length = static_cast<SockLen>(sizeof(loopback));
  if (auto r = bindSocket(*socket, address); !r) return unexpected<Error>(r.error());
  address.length = static_cast<SockLen>(sizeof(address.storage));
  if (::getsockname(socket->handle(), address.data(), &address.length) != 0) {
    int err = lastError();
    return unexpected<Error>(socketError(ErrorCode::Io, "getsockname failed", err));
  }
  if (::connect(socket->handle(), address.data(), address.length) != 0) {
    int err = lastError();
    return unexpected<Error>(socketError(ErrorCode::Io, "connecting the wake-up socket failed", err));
  }
  if (auto r = setBlocking(*socket, false); !r) return unexpected<Error>(r.error());
  wake.wait_ = std::move(*socket);
#else
  int fds[2] = {-1, -1};
#if defined(__linux__)
  const int rc = ::pipe2(fds, O_CLOEXEC | O_NONBLOCK);
#else
  const int rc = ::pipe(fds);
#endif
  if (rc != 0) {
    int err = lastError();
    return unexpected<Error>(socketError(ErrorCode::Io, "pipe() failed", err));
  }
  wake.wait_ = Socket(fds[0]);
  wake.notify_ = Socket(fds[1]);
#if !defined(__linux__)
  if (!makePipeEnd(fds[0]) || !makePipeEnd(fds[1])) {
    int err = lastError();
    return unexpected<Error>(socketError(ErrorCode::Io, "configuring the wake-up pipe failed", err));
  }
#endif
#endif
  return wake;
}

void WakeSignal::signal() const noexcept {
  const char byte = 1;
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  if (wait_.valid()) (void)::send(wait_.handle(), &byte, 1, 0);
#else
  if (notify_.valid()) {
    while (::write(notify_.handle(), &byte, 1) < 0 && isInterrupted(lastError())) {
    }
  }
#endif
}

void WakeSignal::close() noexcept {
  notify_.close();
  wait_.close();
}

int lastError() noexcept {
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

std::string errorToString(int sysError) {
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  char buffer[512];
  DWORD length = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, static_cast<DWORD>(sysError), 0, buffer,
                                  static_cast<DWORD>(sizeof(buffer)), nullptr);
  while (length > 0 && (buffer[length - 1] == '\r' || buffer[length - 1] == '\n' ||
                        buffer[length - 1] == ' ' || buffer[length - 1] == '.')) {
    --length;
  }
  if (length == 0) return "error " + std::to_string(sysError);
  return std::string(buffer, length);
#else
  return std::generic_category().message(sysError);
#endif
}

bool wouldBlock(int sysError) noexcept {
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  return sysError == WSAEWOULDBLOCK;
#else
  return sysError == EAGAIN || sysError == EWOULDBLOCK;
#endif
}

ErrorCode classifyError(int sysError, ErrorCode fallback) noexcept {
  if (isTimeoutError(sysError)) return ErrorCode::Timeout;
  if (isDisconnectError(sysError)) return ErrorCode::Disconnected;
  return fallback;
}

Error socketError(ErrorCode fallback, std::string_view context, int sysError) {
  const ErrorCode code = classifyError(sysError, fallback);
  std::string message(context);
  message += ": ";
  message += code == ErrorCode::Timeout ? std::string("timed out") : errorToString(sysError);
  return makeError(code, std::move(message), sysError);
}

Result<Socket> createSocket(int family, int type) {
#if defined(SOCK_CLOEXEC)
  type |= SOCK_CLOEXEC;
#endif
  SocketHandle handle = ::socket(family, type, 0);
  if (handle == kInvalidSocket) {
    int err = lastError();
    return unexpected<Error>(socketError(ErrorCode::Io, "socket() failed", err));
  }
  Socket socket(handle);
#if !defined(UPDCLIENT_PLATFORM_WINSOCK)
#if !defined(SOCK_CLOEXEC)
  ::fcntl(handle, F_SETFD, FD_CLOEXEC);
#endif
#if defined(SO_NOSIGPIPE)
  int on = 1;
  ::setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
#endif
  return socket;
}

Result<std::vector<SocketAddress>> resolve(std::string_view host, uint16_t port, int type,
                                           bool passive, int family) {
  addrinfo hints{};
  hints.ai_family = family;
  hints.ai_socktype = type;
  hints.ai_flags = AI_NUMERICHOST;
#if defined(AI_NUMERICSERV)
  hints.ai_flags |= AI_NUMERICSERV;
#endif
  if (passive) hints.ai_flags |= AI_PASSIVE;

  std::string hostText(host);
  std::string service = std::to_string(port);
  addrinfo *list = nullptr;
  int rc = ::getaddrinfo(hostText.empty() ? nullptr : hostText.c_str(), service.c_str(), &hints,
                         &list);
  if (rc != 0) {
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
    std::string text = errorToString(rc);
#else
    std::string text = ::gai_strerror(rc);
#endif
    ErrorCode code = (rc == EAI_NONAME || rc == EAI_FAMILY || rc == EAI_SERVICE)
                         ? ErrorCode::InvalidArgument
                         : ErrorCode::Io;
    return fail(code, "cannot resolve '" + hostText + "' (numeric IP address required): " + text);
  }

  std::vector<SocketAddress> out;
  for (const addrinfo *ai = list; ai != nullptr; ai = ai->ai_next) {
    if (ai->ai_addr == nullptr || static_cast<size_t>(ai->ai_addrlen) > sizeof(sockaddr_storage)) {
      continue;
    }
    SocketAddress address;
    std::memcpy(&address.storage, ai->ai_addr, ai->ai_addrlen);
    address.length = static_cast<SockLen>(ai->ai_addrlen);
    out.push_back(address);
  }
  ::freeaddrinfo(list);

  if (out.empty()) {
    return fail(ErrorCode::InvalidArgument, "no usable address for '" + hostText + "'");
  }
  return out;
}

std::string addressToString(const SocketAddress &address) {
  char buffer[128];
  int rc = ::getnameinfo(address.data(), address.length, buffer, sizeof(buffer), nullptr, 0,
                         NI_NUMERICHOST);
  if (rc != 0) return "?";
  return buffer;
}

uint16_t addressPort(const SocketAddress &address) noexcept {
  if (address.storage.ss_family == AF_INET) {
    sockaddr_in in{};
    std::memcpy(&in, &address.storage, sizeof(in));
    return ntohs(in.sin_port);
  }
  if (address.storage.ss_family == AF_INET6) {
    sockaddr_in6 in6{};
    std::memcpy(&in6, &address.storage, sizeof(in6));
    return ntohs(in6.sin6_port);
  }
  return 0;
}

Result<void> setBlocking(const Socket &socket, bool blocking) {
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  u_long mode = blocking ? 0 : 1;
  if (::ioctlsocket(socket.handle(), FIONBIO, &mode) != 0) {
    int err = lastError();
    return unexpected<Error>(socketError(ErrorCode::Io, "ioctlsocket(FIONBIO) failed", err));
  }
#else
  int flags = ::fcntl(socket.handle(), F_GETFL, 0);
  if (flags < 0) {
    int err = lastError();
    return unexpected<Error>(socketError(ErrorCode::Io, "fcntl(F_GETFL) failed", err));
  }
  flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
  if (::fcntl(socket.handle(), F_SETFL, flags) < 0) {
    int err = lastError();
    return unexpected<Error>(socketError(ErrorCode::Io, "fcntl(F_SETFL) failed", err));
  }
#endif
  return {};
}

Result<void> setNoDelay(const Socket &socket) {
  int on = 1;
  if (::setsockopt(socket.handle(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on),
                   sizeof(on)) != 0) {
    int err = lastError();
    return unexpected<Error>(socketError(ErrorCode::Io, "setting TCP_NODELAY failed", err));
  }
  return {};
}

Result<void> setReuseAddress(const Socket &socket) {
  int on = 1;
  if (::setsockopt(socket.handle(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&on),
                   sizeof(on)) != 0) {
    int err = lastError();
    return unexpected<Error>(socketError(ErrorCode::Io, "setting SO_REUSEADDR failed", err));
  }
#if !defined(UPDCLIENT_PLATFORM_WINSOCK) && !defined(__linux__) && defined(SO_REUSEPORT)
  ::setsockopt(socket.handle(), SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
  return {};
}

Result<void> setBroadcast(const Socket &socket) {
  int on = 1;
  if (::setsockopt(socket.handle(), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&on),
                   sizeof(on)) != 0) {
    int err = lastError();
    return unexpected<Error>(socketError(ErrorCode::Io, "setting SO_BROADCAST failed", err));
  }
  return {};
}

Result<void> joinMulticast(const Socket &socket, const SocketAddress &group,
                           std::string_view localInterface) {
  const std::string interfaceText(localInterface);
  if (group.storage.ss_family == AF_INET) {
    ip_mreq request{};
    std::memcpy(&request.imr_multiaddr, &reinterpret_cast<const sockaddr_in *>(&group.storage)->sin_addr,
                sizeof(request.imr_multiaddr));
    if (interfaceText.empty()) {
      request.imr_interface.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, interfaceText.c_str(), &request.imr_interface) != 1) {
      return fail(ErrorCode::InvalidArgument,
                  "multicast interface '" + interfaceText + "' is not a numeric IPv4 address");
    }
    if (::setsockopt(socket.handle(), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     reinterpret_cast<const char *>(&request), sizeof(request)) != 0) {
      int err = lastError();
      return unexpected<Error>(socketError(ErrorCode::Io, "joining multicast group failed", err));
    }
    return {};
  }

  if (group.storage.ss_family == AF_INET6) {
    ipv6_mreq request{};
    std::memcpy(&request.ipv6mr_multiaddr,
                &reinterpret_cast<const sockaddr_in6 *>(&group.storage)->sin6_addr,
                sizeof(request.ipv6mr_multiaddr));
    unsigned index = 0;
    if (!interfaceText.empty()) {
      const auto [ptr, ec] =
          std::from_chars(interfaceText.data(), interfaceText.data() + interfaceText.size(), index);
      if (ec != std::errc{} || ptr != interfaceText.data() + interfaceText.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "multicast interface '" + interfaceText + "' is not an IPv6 interface index");
      }
    }
    request.ipv6mr_interface = index;
    if (::setsockopt(socket.handle(), IPPROTO_IPV6, IPV6_JOIN_GROUP,
                     reinterpret_cast<const char *>(&request), sizeof(request)) != 0) {
      int err = lastError();
      return unexpected<Error>(socketError(ErrorCode::Io, "joining multicast group failed", err));
    }
    return {};
  }

  return fail(ErrorCode::InvalidArgument, "multicast needs an IPv4 or IPv6 group address");
}

Result<void> bindSocket(const Socket &socket, const SocketAddress &address) {
  if (::bind(socket.handle(), address.data(), address.length) != 0) {
    int err = lastError();
    return unexpected<Error>(socketError(
        ErrorCode::Io, "bind to port " + std::to_string(addressPort(address)) + " failed", err));
  }
  return {};
}

Result<uint16_t> localPort(const Socket &socket) {
  SocketAddress address;
  address.length = static_cast<SockLen>(sizeof(address.storage));
  if (::getsockname(socket.handle(), address.data(), &address.length) != 0) {
    int err = lastError();
    return unexpected<Error>(socketError(ErrorCode::Io, "getsockname failed", err));
  }
  return addressPort(address);
}

Result<void> connectWithTimeout(const Socket &socket, const SocketAddress &address,
                                std::chrono::milliseconds timeout, const WakeSignal *wake) {
  if (timeout.count() <= 0) timeout = std::chrono::milliseconds(-1);

  if (auto r = setBlocking(socket, false); !r) return r;

  bool inProgress = false;
  if (::connect(socket.handle(), address.data(), address.length) != 0) {
    int err = lastError();
    if (isInterrupted(err) || isConnectInProgress(err)) {
      inProgress = true;
    } else {
      ErrorCode code = isConnectTimeoutError(err) ? ErrorCode::Timeout : ErrorCode::ConnectFailed;
      return fail(code, "connect to " + addressToString(address) + ":" +
                            std::to_string(addressPort(address)) + " failed: " + errorToString(err),
                  err);
    }
  }

  if (inProgress) {
    auto ready = waitSocket(socket.handle(), WaitFor::Writable, timeout, wake);
    if (!ready) return unexpected<Error>(ready.error());
    if (*ready == WaitResult::Woken) {
      return fail(ErrorCode::Cancelled, "connect to " + addressToString(address) + ":" +
                                            std::to_string(addressPort(address)) + " cancelled");
    }
    if (*ready == WaitResult::TimedOut) {
      return fail(ErrorCode::Timeout, "connect to " + addressToString(address) + ":" +
                                          std::to_string(addressPort(address)) + " timed out after " +
                                          std::to_string(timeout.count()) + " ms");
    }
    int soError = 0;
    SockLen length = static_cast<SockLen>(sizeof(soError));
    if (::getsockopt(socket.handle(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soError),
                     &length) != 0) {
      soError = lastError();
    }
    if (soError != 0) {
      ErrorCode code = isConnectTimeoutError(soError) ? ErrorCode::Timeout : ErrorCode::ConnectFailed;
      return fail(code, "connect to " + addressToString(address) + ":" +
                            std::to_string(addressPort(address)) + " failed: " + errorToString(soError),
                  soError);
    }
  }

  return setBlocking(socket, true);
}

Result<bool> waitReadable(const Socket &socket, std::chrono::milliseconds timeout) {
  return waitSocket(socket.handle(), WaitFor::Readable, timeout);
}

Result<WaitResult> waitFor(const Socket &socket, WaitFor what, std::chrono::milliseconds timeout,
                           const WakeSignal *wake) {
  return waitSocket(socket.handle(), what, timeout, wake);
}

void shutdownBoth(const Socket &socket) noexcept {
  if (!socket.valid()) return;
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
  (void)::shutdown(socket.handle(), SD_BOTH);
#else
  (void)::shutdown(socket.handle(), SHUT_RDWR);
#endif
}

Result<size_t> sendSome(const Socket &socket, std::span<const uint8_t> data) {
  if (data.empty()) return size_t{0};
  const size_t length = std::min(data.size(), kMaxIoChunk);
  for (;;) {
    auto sent = static_cast<std::ptrdiff_t>(
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
        ::send(socket.handle(), reinterpret_cast<const char *>(data.data()), static_cast<int>(length),
               kMsgFlags)
#else
        ::send(socket.handle(), data.data(), length, kMsgFlags)
#endif
    );
    if (sent >= 0) return static_cast<size_t>(sent);
    int err = lastError();
    if (isInterrupted(err)) continue;
    return unexpected<Error>(socketError(ErrorCode::Io, "send failed", err));
  }
}

Result<size_t> recvSome(const Socket &socket, std::span<uint8_t> buffer) {
  if (buffer.empty()) return size_t{0};
  const size_t length = std::min(buffer.size(), kMaxIoChunk);
  for (;;) {
    auto received = static_cast<std::ptrdiff_t>(
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
        ::recv(socket.handle(), reinterpret_cast<char *>(buffer.data()), static_cast<int>(length), 0)
#else
        ::recv(socket.handle(), buffer.data(), length, 0)
#endif
    );
    if (received >= 0) return static_cast<size_t>(received);
    int err = lastError();
    if (isInterrupted(err)) continue;
    return unexpected<Error>(socketError(ErrorCode::Io, "recv failed", err));
  }
}

Result<size_t> sendTo(const Socket &socket, std::span<const uint8_t> data, const SocketAddress &to) {
  const size_t length = std::min(data.size(), kMaxIoChunk);
  for (;;) {
    auto sent = static_cast<std::ptrdiff_t>(
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
        ::sendto(socket.handle(), reinterpret_cast<const char *>(data.data()), static_cast<int>(length),
                 kMsgFlags, to.data(), to.length)
#else
        ::sendto(socket.handle(), data.data(), length, kMsgFlags, to.data(), to.length)
#endif
    );
    if (sent >= 0) return static_cast<size_t>(sent);
    int err = lastError();
    if (isInterrupted(err)) continue;
    return unexpected<Error>(socketError(ErrorCode::Io, "sendto failed", err));
  }
}

Result<size_t> recvFrom(const Socket &socket, std::span<uint8_t> buffer, SocketAddress &from) {
  const size_t length = std::min(buffer.size(), kMaxIoChunk);
  for (;;) {
    from.length = static_cast<SockLen>(sizeof(from.storage));
    auto received = static_cast<std::ptrdiff_t>(
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
        ::recvfrom(socket.handle(), reinterpret_cast<char *>(buffer.data()), static_cast<int>(length),
                   0, from.data(), &from.length)
#else
        ::recvfrom(socket.handle(), buffer.data(), length, 0, from.data(), &from.length)
#endif
    );
    if (received >= 0) return static_cast<size_t>(received);
    int err = lastError();
    if (isInterrupted(err)) continue;
#if defined(UPDCLIENT_PLATFORM_WINSOCK)
    if (err == WSAEMSGSIZE) return length;
#endif
    return unexpected<Error>(socketError(ErrorCode::Io, "recvfrom failed", err));
  }
}

} // namespace updclient::net::platform
