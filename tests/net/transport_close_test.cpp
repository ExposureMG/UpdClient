#include "support/loopback_server.hpp"
#include "support/memory_transport.hpp"
#include "support/mock_transport.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <updclient/net/tcp_transport.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

// close() from another thread must wake a read or write that is blocked on the
// transport, with ErrorCode::Cancelled, long before the transport's own timeout.

using namespace updclient;
using namespace std::chrono_literals;
using ut::Bytes;
using ut::LoopbackServer;
using ut::MemoryPipe;

namespace {

using Clock = std::chrono::steady_clock;

// Long enough that a test only passes if close() did the waking.
constexpr auto kIoTimeout = 20s;
constexpr auto kPrompt = 3s;

// Runs fn on its own thread; join() returns what it returned.
template <class T> class Background {
public:
  template <class Fn>
  explicit Background(Fn fn)
      : thread_([this, fn = std::move(fn)]() mutable {
          result_.emplace(fn());
          done_ = true;
        }) {}
  Background(const Background &) = delete;
  Background &operator=(const Background &) = delete;
  ~Background() {
    if (thread_.joinable()) thread_.join();
  }

  bool done() const noexcept { return done_; }
  T join() {
    thread_.join();
    return std::move(*result_);
  }

private:
  std::optional<T> result_;
  std::atomic<bool> done_{false};
  std::thread thread_;
};

net::Endpoint loopbackEndpoint(uint16_t port) {
  net::Endpoint e;
  e.scheme = "tcp";
  e.host = "127.0.0.1";
  e.port = port;
  e.timeout = 3000ms;
  return e;
}

std::unique_ptr<LoopbackServer> startOrSkip(LoopbackServer::Handler handler) {
  std::string why;
  auto server = LoopbackServer::start(std::move(handler), &why);
  if (!server) SKIP("loopback sockets are not available here: " + why);
  return server;
}

// A peer that keeps the connection open, never reads, and sends `greeting` first.
LoopbackServer::Handler silentPeer(Bytes greeting = {}) {
  return [greeting](ut::ServerConnection &conn, const std::atomic<bool> &stop) {
    if (!greeting.empty()) conn.sendAll(greeting);
    while (!stop) std::this_thread::sleep_for(10ms);
  };
}

net::TransportPtr connectOrFail(uint16_t port) {
  auto transport = net::TcpTransport::connect(loopbackEndpoint(port));
  if (!transport) return nullptr;
  if (!(*transport)->setTimeout(kIoTimeout)) return nullptr;
  return std::move(*transport);
}

} // namespace

TEST(TcpTransportClose, UnblocksAReadFromAnotherThread) {
  auto peerSawClose = std::make_shared<std::atomic<bool>>(false);
  auto server = startOrSkip([peerSawClose](ut::ServerConnection &conn, const std::atomic<bool> &) {
    Bytes in;
    const auto start = Clock::now();
    if (!conn.recvExact(in, 1, 8000) && Clock::now() - start < 7s) *peerSawClose = true;
  });
  auto transport = connectOrFail(server->port());
  REQUIRE(transport != nullptr);

  Background<Result<size_t>> reader([&] {
    Bytes buffer(16);
    return transport->readSome(buffer);
  });
  std::this_thread::sleep_for(200ms);
  REQUIRE(!reader.done());

  const auto start = Clock::now();
  transport->close();
  const auto result = reader.join();
  CHECK(Clock::now() - start < kPrompt);
  CHECK_ERR(result, ErrorCode::Cancelled);
  CHECK(!transport->isOpen());

  server.reset();
  CHECK(peerSawClose->load());
}

TEST(TcpTransportClose, UnblocksAWriteToAFullSocketBuffer) {
  auto server = startOrSkip(silentPeer());
  auto transport = connectOrFail(server->port());
  REQUIRE(transport != nullptr);

  // Far more than the loopback send and receive buffers hold together.
  const Bytes payload(32u << 20, 0xA5);
  Background<Result<void>> writer([&] { return transport->writeAll(payload); });
  std::this_thread::sleep_for(300ms);
  REQUIRE(!writer.done());

  const auto start = Clock::now();
  transport->close();
  const auto result = writer.join();
  CHECK(Clock::now() - start < kPrompt);
  CHECK_ERR(result, ErrorCode::Cancelled);
}

TEST(TcpTransportClose, ReadExactAfterPartialDataIsCancelledNotDisconnected) {
  auto server = startOrSkip(silentPeer(ut::bytesOf("abc")));
  auto transport = connectOrFail(server->port());
  REQUIRE(transport != nullptr);

  Background<Result<void>> reader([&] {
    Bytes buffer(10);
    return transport->readExact(buffer);
  });
  std::this_thread::sleep_for(200ms);
  REQUIRE(!reader.done());
  transport->close();
  CHECK_ERR(reader.join(), ErrorCode::Cancelled);
}

TEST(TcpTransportClose, ReadUntilEofDoesNotReturnTruncatedDataAsSuccess) {
  auto server = startOrSkip(silentPeer(ut::bytesOf("abc")));
  auto transport = connectOrFail(server->port());
  REQUIRE(transport != nullptr);

  Background<Result<std::vector<uint8_t>>> reader([&] { return transport->readUntilEof(1 << 20); });
  std::this_thread::sleep_for(200ms);
  REQUIRE(!reader.done());
  transport->close();
  CHECK_ERR(reader.join(), ErrorCode::Cancelled);
}

TEST(TcpTransportClose, DoubleCloseIsHarmless) {
  auto server = startOrSkip(silentPeer());
  auto transport = connectOrFail(server->port());
  REQUIRE(transport != nullptr);

  transport->close();
  transport->close();
  CHECK(!transport->isOpen());
  Bytes buffer(4);
  CHECK_ERR(transport->readSome(buffer), ErrorCode::NotConnected);
  CHECK_ERR(transport->writeSome(buffer), ErrorCode::NotConnected);
  CHECK_ERR(transport->readExact(buffer), ErrorCode::NotConnected);
  CHECK_ERR(transport->setTimeout(10ms), ErrorCode::NotConnected);
}

TEST(TcpTransportClose, ConcurrentClosesWhileAReadIsBlocked) {
  auto server = startOrSkip(silentPeer());
  auto transport = connectOrFail(server->port());
  REQUIRE(transport != nullptr);

  Background<Result<size_t>> reader([&] {
    Bytes buffer(16);
    return transport->readSome(buffer);
  });
  std::this_thread::sleep_for(100ms);

  std::vector<std::thread> closers;
  for (int i = 0; i < 4; ++i) closers.emplace_back([&] { transport->close(); });
  for (auto &closer : closers) closer.join();
  CHECK_ERR(reader.join(), ErrorCode::Cancelled);
  CHECK(!transport->isOpen());
}

// Closes at varying points of a busy read or write loop. Every call must end with
// Cancelled or, if it started after close(), NotConnected; never with a timeout,
// a hang or a bogus end of stream. Meant to be run under ThreadSanitizer too.
TEST(TcpTransportClose, CloseRacingWithIoEndsEveryCallCleanly) {
  for (int round = 0; round < 16; ++round) {
    const bool writing = round % 2 == 1;
    auto server = startOrSkip(silentPeer());
    auto transport = connectOrFail(server->port());
    REQUIRE(transport != nullptr);

    Background<Error> worker([&] {
      const Bytes chunk(256 * 1024, 0x11);
      Bytes buffer(64);
      for (;;) {
        if (writing) {
          auto r = transport->writeSome(chunk);
          if (!r) return r.error();
        } else {
          auto r = transport->readSome(buffer);
          if (!r) return r.error();
          if (*r == 0) return makeError(ErrorCode::Unknown, "unexpected end of stream");
        }
      }
    });
    std::this_thread::sleep_for(std::chrono::microseconds(250 * round));
    transport->close();
    const Error error = worker.join();
    const bool closedLocally = error.code == ErrorCode::Cancelled || error.code == ErrorCode::NotConnected;
    CHECK_EQ(closedLocally ? std::string("closed locally") : formatError(error), std::string("closed locally"));
  }
}

TEST(TcpTransportClose, TimeoutStillAppliesWhenNobodyCloses) {
  auto server = startOrSkip(silentPeer());
  auto transport = net::TcpTransport::connect(loopbackEndpoint(server->port()));
  REQUIRE_OK(transport);
  REQUIRE_OK((*transport)->setTimeout(150ms));
  Bytes buffer(4);
  const auto start = Clock::now();
  CHECK_ERR((*transport)->readSome(buffer), ErrorCode::Timeout);
  CHECK(Clock::now() - start >= 100ms);
  CHECK((*transport)->isOpen());
}

TEST(MemoryPipe, RoundTripAndEndOfStreamAfterPeerClose) {
  auto pipe = MemoryPipe::create();
  REQUIRE_OK(pipe.client->writeAll(ut::bytesOf("ping")));
  Bytes in(4);
  REQUIRE_OK(pipe.server->readExact(in));
  CHECK_EQ(ut::textOf(in), std::string("ping"));

  REQUIRE_OK(pipe.server->writeAll(ut::bytesOf("pong")));
  pipe.server->close();
  auto rest = pipe.client->readUntilEof(100);
  REQUIRE_OK(rest);
  CHECK_EQ(ut::textOf(*rest), std::string("pong"));
  CHECK_ERR(pipe.client->writeAll(ut::bytesOf("x")), ErrorCode::Disconnected);
}

TEST(MemoryPipe, CapacityForcesShortWritesAndTimeouts) {
  auto pipe = MemoryPipe::create(3, 50ms);
  auto n = pipe.client->writeSome(ut::bytesOf("abcdef"));
  REQUIRE_OK(n);
  CHECK_EQ(*n, size_t{3});
  CHECK_ERR(pipe.client->writeSome(ut::bytesOf("d")), ErrorCode::Timeout);
  Bytes in(8);
  auto got = pipe.server->readSome(in);
  REQUIRE_OK(got);
  CHECK_EQ(*got, size_t{3});
  CHECK_ERR(pipe.server->readSome(in), ErrorCode::Timeout);
}

TEST(MemoryPipe, CloseUnblocksAReadFromAnotherThread) {
  auto pipe = MemoryPipe::create(64, kIoTimeout);
  Background<Result<size_t>> reader([&] {
    Bytes buffer(8);
    return pipe.client->readSome(buffer);
  });
  REQUIRE(pipe.client->waitUntilBlocked());
  const auto start = Clock::now();
  pipe.client->close();
  CHECK_ERR(reader.join(), ErrorCode::Cancelled);
  CHECK(Clock::now() - start < kPrompt);

  Bytes buffer(8);
  CHECK_ERR(pipe.client->readSome(buffer), ErrorCode::NotConnected);
  CHECK(pipe.server->isOpen());
}

TEST(MemoryPipe, CloseUnblocksAWriteToAFullPipe) {
  auto pipe = MemoryPipe::create(16, kIoTimeout);
  const Bytes payload(1000, 0x42);
  Background<Result<void>> writer([&] { return pipe.client->writeAll(payload); });
  REQUIRE(pipe.client->waitUntilBlocked());
  pipe.client->close();
  CHECK_ERR(writer.join(), ErrorCode::Cancelled);
}

TEST(MemoryPipe, ReadExactInterruptedBetweenChunksIsCancelled) {
  auto pipe = MemoryPipe::create(64, kIoTimeout);
  REQUIRE_OK(pipe.server->writeAll(ut::bytesOf("abc")));
  Background<Result<void>> reader([&] {
    Bytes buffer(10);
    return pipe.client->readExact(buffer);
  });
  REQUIRE(pipe.client->waitUntilBlocked());
  pipe.client->close();
  CHECK_ERR(reader.join(), ErrorCode::Cancelled);
}

TEST(MemoryPipe, DoubleCloseIsHarmless) {
  auto pipe = MemoryPipe::create();
  pipe.client->close();
  pipe.client->close();
  CHECK(!pipe.client->isOpen());
  Bytes buffer(1);
  CHECK_ERR(pipe.client->writeSome(buffer), ErrorCode::NotConnected);
  Bytes in(1);
  auto eof = pipe.server->readSome(in);
  REQUIRE_OK(eof);
  CHECK_EQ(*eof, size_t{0});
}

TEST(MockTransport, CloseFromAnotherThreadIsSeenByTheNextCall) {
  auto script = ut::MockScript::create();
  script->reply("data");
  auto transport = script->transport();
  std::thread closer([&] { transport->close(); });
  closer.join();
  CHECK(!transport->isOpen());
  CHECK(script->closed());
  Bytes buffer(4);
  CHECK_ERR(transport->readSome(buffer), ErrorCode::NotConnected);
}
