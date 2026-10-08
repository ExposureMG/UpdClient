#include "support/loopback_server.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <net/tcp_transport.hpp>
#include <net/transport_registry.hpp>
#include <protocols/updserver/client.hpp>
#include <protocols/xell/client.hpp>
#include <updclient.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#endif

using namespace updclient;
using namespace std::chrono_literals;
using ut::Bytes;
using ut::LoopbackServer;

namespace {

net::Endpoint loopbackEndpoint(uint16_t port, std::chrono::milliseconds timeout = 3000ms) {
  net::Endpoint e;
  e.scheme = "tcp";
  e.host = "127.0.0.1";
  e.port = port;
  e.timeout = timeout;
  return e;
}

std::unique_ptr<LoopbackServer> startOrSkip(LoopbackServer::Handler handler) {
  std::string why;
  auto server = LoopbackServer::start(std::move(handler), &why);
  if (!server) SKIP("loopback sockets are not available here: " + why);
  return server;
}

// Everything the server side of a test saw, readable after the server is destroyed.
struct Captured {
  std::mutex mutex;
  Bytes data;
};

} // namespace

TEST(TcpTransport, RoundTripOverLoopback) {
  auto server = startOrSkip([](ut::ServerConnection &conn, const std::atomic<bool> &) {
    Bytes in;
    if (!conn.recvExact(in, 5)) return;
    if (ut::textOf(in) == "hello") conn.sendAll(ut::bytesOf("world"));
  });

  auto transport = net::TcpTransport::connect(loopbackEndpoint(server->port()));
  REQUIRE_OK(transport);
  auto &t = **transport;
  CHECK(t.isOpen());
  CHECK(t.describe().find("127.0.0.1") != std::string::npos);
  CHECK(t.describe().find(std::to_string(server->port())) != std::string::npos);

  const Bytes hello = ut::bytesOf("hello");
  REQUIRE_OK(t.writeAll(hello));
  Bytes reply(5);
  REQUIRE_OK(t.readExact(reply));
  CHECK_EQ(ut::textOf(reply), std::string("world"));

  Bytes more(1);
  auto eof = t.readSome(more);
  REQUIRE_OK(eof);
  CHECK_EQ(*eof, size_t{0});

  t.close();
  CHECK(!t.isOpen());
  CHECK_ERR(t.readSome(more), ErrorCode::NotConnected);
  CHECK_ERR(t.writeAll(hello), ErrorCode::NotConnected);
  CHECK_ERR(t.setTimeout(100ms), ErrorCode::NotConnected);
}

TEST(TcpTransport, ConnectOverloadWithHostAndPort) {
  auto server = startOrSkip([](ut::ServerConnection &conn, const std::atomic<bool> &) { conn.sendAll(ut::bytesOf("x")); });
  auto transport = net::TcpTransport::connect("127.0.0.1", server->port(), 3000ms);
  REQUIRE_OK(transport);
  Bytes one(1);
  REQUIRE_OK((*transport)->readExact(one));
  CHECK_EQ(one[0], 'x');
}

TEST(TcpTransport, LargeTransferAndLimit) {
  const Bytes payload = ut::patternBytes(300000, 99);
  auto handler = [payload](ut::ServerConnection &conn, const std::atomic<bool> &) { conn.sendAll(payload); };
  {
    auto server = startOrSkip(handler);
    auto transport = net::TcpTransport::connect(loopbackEndpoint(server->port()));
    REQUIRE_OK(transport);
    auto data = (*transport)->readUntilEof(1 << 20);
    REQUIRE_OK(data);
    CHECK_EQ(*data, payload);
  }
  {
    auto server = startOrSkip(handler);
    auto transport = net::TcpTransport::connect(loopbackEndpoint(server->port()));
    REQUIRE_OK(transport);
    CHECK_ERR((*transport)->readUntilEof(1000), ErrorCode::LimitExceeded);
  }
}

TEST(TcpTransport, ReadTimeout) {
  auto server = startOrSkip([](ut::ServerConnection &, const std::atomic<bool> &stop) {
    for (int i = 0; i < 100 && !stop; ++i) std::this_thread::sleep_for(50ms);
  });
  auto transport = net::TcpTransport::connect(loopbackEndpoint(server->port()));
  REQUIRE_OK(transport);
  REQUIRE_OK((*transport)->setTimeout(150ms));

  Bytes buffer(16);
  const auto start = std::chrono::steady_clock::now();
  const auto r = (*transport)->readSome(buffer);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(elapsed >= 100ms);
  CHECK(elapsed < 4s);
  CHECK((*transport)->isOpen());
}

TEST(TcpTransport, EndpointTimeoutBecomesTheInitialReadTimeout) {
  auto server = startOrSkip([](ut::ServerConnection &, const std::atomic<bool> &stop) {
    for (int i = 0; i < 100 && !stop; ++i) std::this_thread::sleep_for(50ms);
  });
  auto transport = net::TcpTransport::connect(loopbackEndpoint(server->port(), 150ms));
  REQUIRE_OK(transport);
  Bytes buffer(16);
  CHECK_ERR((*transport)->readExact(buffer), ErrorCode::Timeout);
}

TEST(TcpTransport, ReadTimeoutAfterPartialData) {
  auto server = startOrSkip([](ut::ServerConnection &conn, const std::atomic<bool> &stop) {
    conn.sendAll(ut::bytesOf("abc"));
    for (int i = 0; i < 100 && !stop; ++i) std::this_thread::sleep_for(50ms);
  });
  auto transport = net::TcpTransport::connect(loopbackEndpoint(server->port(), 200ms));
  REQUIRE_OK(transport);
  Bytes buffer(10);
  CHECK_ERR((*transport)->readExact(buffer), ErrorCode::Timeout);
}

TEST(TcpTransport, ConnectionRefusedIsConnectFailed) {
  const uint16_t port = ut::unusedLoopbackPort();
  if (port == 0) SKIP("cannot reserve a loopback port here");
  const auto r = net::TcpTransport::connect(loopbackEndpoint(port, 2000ms));
  REQUIRE_ERR(r, ErrorCode::ConnectFailed);
  CHECK(r.error().sysError != 0);
  CHECK(r.error().message.find("127.0.0.1") != std::string::npos);
}

#if !defined(_WIN32)
// On Linux a listener whose accept queue is full drops further SYNs, so a connect()
// to it hangs until the timeout. The transport reports that as a timeout; both
// Timeout and ConnectFailed are accepted so the test follows the documented
// "connect failed" contract either way.
TEST(TcpTransport, ConnectTimeoutAgainstAFullBacklog) {
  ut::SocketHandle listener(::socket(AF_INET, SOCK_STREAM, 0));
  if (!listener.valid()) SKIP("cannot create a socket here");
  sockaddr_in address = ut::loopbackAddress(0);
  if (::bind(listener.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) SKIP("cannot bind here");
  if (::listen(listener.get(), 0) != 0) SKIP("cannot listen here");
  socklen_t length = sizeof(address);
  if (::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&address), &length) != 0) SKIP("getsockname failed");
  const uint16_t port = ntohs(address.sin_port);

  std::vector<ut::SocketHandle> fillers;
  for (int i = 0; i < 6; ++i) {
    ut::SocketHandle filler(::socket(AF_INET, SOCK_STREAM, 0));
    if (!filler.valid()) break;
    ::fcntl(filler.get(), F_SETFL, ::fcntl(filler.get(), F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in target = ut::loopbackAddress(port);
    (void)::connect(filler.get(), reinterpret_cast<sockaddr *>(&target), sizeof(target));
    fillers.push_back(std::move(filler));
  }
  std::this_thread::sleep_for(100ms);

  const auto start = std::chrono::steady_clock::now();
  const auto r = net::TcpTransport::connect(loopbackEndpoint(port, 300ms));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  if (r.has_value()) SKIP("the kernel still accepted the connection; cannot simulate a connect timeout");
  CHECK(r.error().code == ErrorCode::Timeout || r.error().code == ErrorCode::ConnectFailed);
  CHECK(elapsed < 5s);
}

TEST(TcpTransport, AStopEndsAConnectStuckOnAFullBacklog) {
  auto stalled = ut::StalledListener::start();
  if (!stalled) SKIP("cannot set up a full accept queue here");

  std::stop_source source;
  std::thread stopper([&] {
    std::this_thread::sleep_for(100ms);
    source.request_stop();
  });
  const auto start = std::chrono::steady_clock::now();
  const auto r = net::TcpTransport::connect(loopbackEndpoint(stalled->port(), 5000ms), source.get_token());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  stopper.join();

  if (r.has_value()) SKIP("the kernel still accepted the connection; cannot simulate a stuck connect");
  CHECK(r.error().code == ErrorCode::Cancelled);
  CHECK(elapsed < 1s);
}
#endif

TEST(TcpTransport, AStoppedTokenFailsBeforeConnecting) {
  auto server = startOrSkip([](ut::ServerConnection &, const std::atomic<bool> &) {});
  std::stop_source source;
  source.request_stop();
  const auto r = net::TcpTransport::connect(loopbackEndpoint(server->port()), source.get_token());
  CHECK_ERR(r, ErrorCode::Cancelled);
}

TEST(TcpTransport, ATokenThatIsNeverStoppedConnectsNormally) {
  auto server = startOrSkip([](ut::ServerConnection &, const std::atomic<bool> &) {});
  std::stop_source source;
  const auto r = net::TcpTransport::connect(loopbackEndpoint(server->port()), source.get_token());
  REQUIRE_OK(r);
  source.request_stop(); // after the connect: must not disturb the transport
  CHECK((*r)->isOpen());
}

TEST(TcpTransport, ConnectsByHostName) {
  auto server = startOrSkip([](ut::ServerConnection &, const std::atomic<bool> &) {});
  net::Endpoint endpoint = loopbackEndpoint(server->port());
  endpoint.host = "localhost";
  const auto r = net::TcpTransport::connect(endpoint);
  if (!r.has_value() && r.error().code != ErrorCode::Timeout) {
    if (r.error().code == ErrorCode::InvalidArgument || r.error().code == ErrorCode::ConnectFailed) {
      SKIP("'localhost' does not resolve to an IPv4 address here: " + r.error().message);
    }
  }
  REQUIRE_OK(r);
  CHECK((*r)->describe().find("127.0.0.1") != std::string::npos);
}

TEST(TcpTransport, UnknownHostNameFailsWithinTheTimeout) {
  net::Endpoint endpoint = loopbackEndpoint(49, 2000ms);
  endpoint.host = "no-such-host.invalid"; // RFC 6761: never resolves
  const auto start = std::chrono::steady_clock::now();
  const auto r = net::TcpTransport::connect(endpoint);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  REQUIRE(!r.has_value());
  // NXDOMAIN is InvalidArgument; a machine without a resolver reports its failure instead.
  CHECK(r.error().code == ErrorCode::InvalidArgument || r.error().code == ErrorCode::ConnectFailed ||
        r.error().code == ErrorCode::Timeout);
  CHECK(elapsed < 4s);
}

TEST(TcpTransport, AStopEndsAHostNameLookup) {
  net::Endpoint endpoint = loopbackEndpoint(49, 30000ms);
  endpoint.host = "no-such-host.invalid";
  std::stop_source source;
  source.request_stop();
  CHECK_ERR(net::TcpTransport::connect(endpoint, source.get_token()), ErrorCode::Cancelled);
}

// The lookup thread may still run when its connect returns; it owns what it
// touches, the socket runtime included. Stops land before, during and after lookups.
TEST(TcpTransport, ALookupThatOutlivesItsConnectIsHarmless) {
  for (int i = 0; i < 50; ++i) {
    net::Endpoint endpoint = loopbackEndpoint(49, 30000ms);
    endpoint.host = "no-such-host-" + std::to_string(i) + ".invalid";
    std::stop_source source;
    std::thread stopper([&source, i] {
      std::this_thread::sleep_for(std::chrono::microseconds(100 * (i % 10)));
      source.request_stop();
    });
    const auto r = net::TcpTransport::connect(endpoint, source.get_token());
    stopper.join();
    CHECK(!r.has_value());
  }
}

TEST(TcpTransport, RejectsEndpointWithoutPort) {
  net::Endpoint e;
  e.scheme = "tcp";
  e.host = "127.0.0.1";
  CHECK_ERR(net::TcpTransport::connect(e), ErrorCode::InvalidArgument);
}

TEST(TcpTransport, NegativeTimeoutIsRejected) {
  auto server = startOrSkip([](ut::ServerConnection &, const std::atomic<bool> &stop) {
    for (int i = 0; i < 40 && !stop; ++i) std::this_thread::sleep_for(50ms);
  });
  auto transport = net::TcpTransport::connect(loopbackEndpoint(server->port()));
  REQUIRE_OK(transport);
  CHECK_ERR((*transport)->setTimeout(-1ms), ErrorCode::InvalidArgument);
  CHECK_OK((*transport)->setTimeout(0ms));
}

TEST(TcpTransport, WritingToAClosedPeerFailsWithoutKillingTheProcess) {
  auto server = startOrSkip([](ut::ServerConnection &conn, const std::atomic<bool> &) { conn.close(); });
  auto transport = net::TcpTransport::connect(loopbackEndpoint(server->port()));
  REQUIRE_OK(transport);

  const Bytes chunk(64 * 1024, 0x5A);
  Result<void> last;
  for (int i = 0; i < 400 && last.has_value(); ++i) {
    last = (*transport)->writeAll(chunk);
    if (last.has_value()) std::this_thread::sleep_for(5ms);
  }
  REQUIRE(!last.has_value());
  CHECK(last.error().code == ErrorCode::Disconnected || last.error().code == ErrorCode::Io);
}

TEST(TcpTransport, ReadAfterPeerResetOrCloseIsEofOrDisconnected) {
  auto server = startOrSkip([](ut::ServerConnection &conn, const std::atomic<bool> &) { conn.close(); });
  auto transport = net::TcpTransport::connect(loopbackEndpoint(server->port()));
  REQUIRE_OK(transport);
  Bytes buffer(8);
  CHECK_ERR((*transport)->readExact(buffer), ErrorCode::Disconnected);
}

TEST(TcpTransport, RegisteredTcpSchemeConnectsThroughTheRegistry) {
  registerBuiltins();
  auto server = startOrSkip([](ut::ServerConnection &conn, const std::atomic<bool> &) { conn.sendAll(ut::bytesOf("ok")); });
  auto transport = net::TransportRegistry::instance().connect(loopbackEndpoint(server->port()));
  REQUIRE_OK(transport);
  Bytes two(2);
  REQUIRE_OK((*transport)->readExact(two));
  CHECK_EQ(ut::textOf(two), std::string("ok"));
}

TEST(TcpEndToEnd, UpdServerClientTalksToALoopbackServer) {
  registerBuiltins();
  auto captured = std::make_shared<Captured>();
  auto server = startOrSkip([captured](ut::ServerConnection &conn, const std::atomic<bool> &) {
    Bytes in;
    if (!conn.recvExact(in, 4)) return;
    {
      std::lock_guard<std::mutex> lock(captured->mutex);
      captured->data = in;
    }
    conn.sendAll(ut::be32(0x00010203));
  });

  auto client = updserver::UpdServerClient::connect(loopbackEndpoint(server->port()));
  REQUIRE_OK(client);
  auto version = client->getVersion();
  REQUIRE_OK(version);
  CHECK_EQ(*version, std::string("1.2.3"));
  server.reset();
  std::lock_guard<std::mutex> lock(captured->mutex);
  CHECK_EQ(captured->data, (Bytes{0, 0, 0, 26}));
}

TEST(TcpEndToEnd, XellClientDownloadsADumpWithoutContentLengthOverRealSockets) {
  registerBuiltins();
  for (size_t size : {size_t{40000}, size_t{16384 + 1}}) {
    const Bytes dump = ut::patternBytes(size, static_cast<uint32_t>(size));
    auto server = startOrSkip([dump](ut::ServerConnection &conn, const std::atomic<bool> &) {
      Bytes request;
      Bytes byte(1);
      while (request.size() < 4096) {
        if (!conn.recvExact(byte, 1)) return;
        request.push_back(byte[0]);
        const std::string text = ut::textOf(request);
        if (text.size() >= 4 && text.compare(text.size() - 4, 4, "\r\n\r\n") == 0) break;
      }
      Bytes reply = ut::bytesOf("HTTP/1.0 200 OK\r\nConnection: close\r\n\r\n");
      ut::append(reply, dump);
      conn.sendAll(reply);
    });

    ut::TempDir dir;
    REQUIRE(dir.ok());
    auto client = xell::XellClient::forEndpoint(loopbackEndpoint(server->port()));
    REQUIRE_OK(client.dumpFlash(dir.file("flash.bin")));
    auto onDisk = ut::readFile(dir.file("flash.bin"));
    REQUIRE(onDisk.has_value());
    CHECK_EQ(onDisk->size(), size);
    CHECK_EQ(*onDisk, dump);
  }
}
