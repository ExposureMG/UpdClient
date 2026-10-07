#pragma once

// Private to the library: never include from anything under include/.
// All Winsock / BSD socket differences are confined to this header and its .cpp.

#include <core/error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32) && !defined(__CYGWIN__)
#define UPDCLIENT_PLATFORM_WINSOCK 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace updclient::net::platform {

#if defined(UPDCLIENT_PLATFORM_WINSOCK)
using SocketHandle = SOCKET;
using SockLen = int;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
using SockLen = socklen_t;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

// Address family used for resolution until IPv6 support is enabled.
inline constexpr int kDefaultFamily = AF_INET;

struct SocketAddress {
  sockaddr_storage storage{};
  SockLen length = 0;

  sockaddr *data() noexcept { return reinterpret_cast<sockaddr *>(&storage); }
  const sockaddr *data() const noexcept { return reinterpret_cast<const sockaddr *>(&storage); }
};

// Refcounted Winsock initialisation (no-op elsewhere). Hold one for the lifetime
// of any socket; declare it before the Socket member so it is destroyed last.
class RuntimeGuard {
public:
  static Result<RuntimeGuard> acquire();

  RuntimeGuard(RuntimeGuard &&other) noexcept;
  RuntimeGuard &operator=(RuntimeGuard &&other) noexcept;
  RuntimeGuard(const RuntimeGuard &) = delete;
  RuntimeGuard &operator=(const RuntimeGuard &) = delete;
  ~RuntimeGuard();

private:
  explicit RuntimeGuard(bool active) noexcept : active_(active) {}
  void release() noexcept;

  bool active_ = false;
};

class Socket {
public:
  Socket() = default;
  explicit Socket(SocketHandle handle) noexcept : handle_(handle) {}
  Socket(Socket &&other) noexcept;
  Socket &operator=(Socket &&other) noexcept;
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
  ~Socket();

  bool valid() const noexcept { return handle_ != kInvalidSocket; }
  SocketHandle handle() const noexcept { return handle_; }
  void close() noexcept;

private:
  SocketHandle handle_ = kInvalidSocket;
};

// Wakes a waitFor() that another thread is blocked in. Once signalled it stays
// signalled. POSIX: a non-blocking pipe. Winsock, whose select() takes only
// sockets: a UDP socket connected to itself on 127.0.0.1.
class WakeSignal {
public:
  static Result<WakeSignal> create();

  WakeSignal() = default;

  bool valid() const noexcept { return wait_.valid(); }
  SocketHandle waitHandle() const noexcept { return wait_.handle(); }
  // Safe to call from any thread, concurrently with waitFor(); never blocks.
  void signal() const noexcept;
  void close() noexcept;

private:
  Socket wait_;
  Socket notify_;
};

enum class WaitFor { Readable, Writable };
enum class WaitResult { Ready, TimedOut, Woken };

int lastError() noexcept;
std::string errorToString(int sysError);
// The non-blocking operation would have had to wait.
bool wouldBlock(int sysError) noexcept;
// Maps an OS error to a library code; timeouts and lost connections get their
// own codes, everything else gets fallback.
ErrorCode classifyError(int sysError, ErrorCode fallback) noexcept;
Error socketError(ErrorCode fallback, std::string_view context, int sysError);

Result<Socket> createSocket(int family, int type);
// Numeric hosts only. An empty host with passive set yields the wildcard address.
Result<std::vector<SocketAddress>> resolve(std::string_view host, uint16_t port, int type,
                                           bool passive, int family = kDefaultFamily);
std::string addressToString(const SocketAddress &address);
uint16_t addressPort(const SocketAddress &address) noexcept;

Result<void> setBlocking(const Socket &socket, bool blocking);
Result<void> setNoDelay(const Socket &socket);
Result<void> setReuseAddress(const Socket &socket);
Result<void> setBroadcast(const Socket &socket);
// group must be a multicast address of the socket's family. localInterface is a local
// IPv4 address (AF_INET) or an interface index (AF_INET6); empty selects the default.
Result<void> joinMulticast(const Socket &socket, const SocketAddress &group,
                           std::string_view localInterface);
Result<void> bindSocket(const Socket &socket, const SocketAddress &address);
Result<uint16_t> localPort(const Socket &socket);

// A non-positive timeout waits indefinitely. Fails with ErrorCode::Timeout. A signalled
// wake (which may be null) ends the wait at once with ErrorCode::Cancelled.
Result<void> connectWithTimeout(const Socket &socket, const SocketAddress &address,
                                std::chrono::milliseconds timeout, const WakeSignal *wake = nullptr);
// Zero polls without blocking; a negative timeout waits indefinitely. true if readable.
Result<bool> waitReadable(const Socket &socket, std::chrono::milliseconds timeout);
// As waitReadable, for either direction, and returns Woken as soon as wake (which
// may be null) is signalled. Ready also covers error and hang-up conditions: the
// next send or recv reports them.
Result<WaitResult> waitFor(const Socket &socket, WaitFor what, std::chrono::milliseconds timeout,
                           const WakeSignal *wake);
// Disables both directions; a peer sees end of stream. Errors are ignored.
void shutdownBoth(const Socket &socket) noexcept;

// Return 0 only for an empty buffer (send) or orderly shutdown (recv).
Result<size_t> sendSome(const Socket &socket, std::span<const uint8_t> data);
Result<size_t> recvSome(const Socket &socket, std::span<uint8_t> buffer);
Result<size_t> sendTo(const Socket &socket, std::span<const uint8_t> data, const SocketAddress &to);
Result<size_t> recvFrom(const Socket &socket, std::span<uint8_t> buffer, SocketAddress &from);

} // namespace updclient::net::platform
