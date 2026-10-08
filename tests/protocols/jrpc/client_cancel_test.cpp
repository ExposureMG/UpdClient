#include "protocols/jrpc/client_fake.hpp"
#include "support/memory_transport.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/jrpc/client.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// close() and cancel() from another thread, a second call while one is running, and the
// real timeouts, over the blocking in-memory transport: the console runs on its own
// thread, so every wait is a real one.

using namespace updclient;
using namespace updclient::jrpc;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

// The console end of the pipe, driven by a script on its own thread.
class PipeConsole {
public:
  using Script = std::function<void(PipeConsole &)>;

  PipeConsole(std::unique_ptr<ut::MemoryTransport> end, Script script, bool banner = true) : end_(std::move(end)) {
    thread_ = std::thread([this, script = std::move(script), banner] {
      if (!banner || send("JRPC2 connected\r\n")) script(*this);
    });
  }
  ~PipeConsole() {
    stop = true;
    end_->close();
    if (thread_.joinable()) thread_.join();
  }

  bool send(std::string_view text) { return static_cast<bool>(end_->writeAll(ut::bytesOf(text))); }

  std::optional<std::string> readLine() {
    std::string line;
    uint8_t c = 0;
    for (;;) {
      auto n = end_->readSome(std::span<uint8_t>(&c, 1));
      if (!n || *n == 0) return std::nullopt;
      if (c == '\n') break;
      line.push_back(static_cast<char>(c));
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    record(line);
    return line;
  }

  // Reads (and records) lines until the client closes its end.
  void waitForClose() {
    while (!stop && readLine()) {
    }
  }

  // Joins the script's thread; it ends by itself once the client has closed.
  void finish() {
    if (thread_.joinable()) thread_.join();
  }

  std::vector<std::string> lines() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
  }

  std::atomic<bool> stop{false};

private:
  void record(const std::string &line) {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(line);
  }

  std::unique_ptr<ut::MemoryTransport> end_;
  std::thread thread_;
  mutable std::mutex mutex_;
  std::vector<std::string> lines_;
};

// A flag one thread waits on and another sets.
class Gate {
public:
  void open() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    changed_.notify_all();
  }
  bool wait(std::chrono::milliseconds limit = 10s) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, limit, [&] { return open_; });
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool open_ = false;
};

ClientOptions patientOptions() {
  ClientOptions options;
  options.bannerTimeout = 20s;
  options.idleTimeout = 20s;
  options.callTimeout = 0ms;
  options.byeTimeout = 50ms;
  return options;
}

// Calls cancel once `blocked` calls on the pipe are waiting.
std::thread whenBlocked(const ut::MemoryTransport *probe, size_t blocked, std::function<void()> action,
                        std::atomic<bool> &sawBlocked) {
  return std::thread([probe, blocked, action = std::move(action), &sawBlocked] {
    sawBlocked = probe->waitUntilBlocked(blocked, 10s);
    action();
  });
}

long long msSince(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

bool hasLine(const std::vector<std::string> &lines, const std::string &line) {
  return std::find(lines.begin(), lines.end(), line) != lines.end();
}

// Reads one command, then holds the connection open without a word.
void hearAndStay(PipeConsole &c) {
  if (!c.readLine()) return;
  c.waitForClose();
}

} // namespace

// --- cancel() ----------------------------------------------------------------------

TEST(JrpcClientCancel, CancelEndsACallThatIsWaiting) {
  auto pipe = ut::MemoryPipe::create();
  const auto *probe = pipe.client.get();
  PipeConsole console(std::move(pipe.server), hearAndStay);
  auto client = JrpcClient::attach(std::move(pipe.client), patientOptions());
  REQUIRE_OK(client);

  std::atomic<bool> sawBlocked{false};
  auto canceller = whenBlocked(probe, 2, [&] { client->cancel(); }, sawBlocked);
  const auto start = Clock::now();
  auto r = client->call(jt::intCall());
  canceller.join();
  CHECK(sawBlocked);
  REQUIRE_ERR(r, ErrorCode::Cancelled);
  CHECK(msSince(start) < 5000);
  CHECK(!client->isConnected());
  CHECK(client->lastDelivery()->delivery == Delivery::Sent);
  // The console may still be running the call; until reconnect() nothing is sent.
  CHECK_ERR(client->call(jt::intCall()), ErrorCode::Cancelled);
  CHECK_ERR(client->rawCommand("x"), ErrorCode::Cancelled);
  console.finish();
  // No Bye after a cancel: only the one command was read.
  CHECK_EQ(console.lines().size(), size_t{1});
  CHECK(!hasLine(console.lines(), "Bye"));
}

TEST(JrpcClientCancel, CancelWhileIdleFailsTheNextCall) {
  auto pipe = ut::MemoryPipe::create();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) { c.waitForClose(); });
  auto client = JrpcClient::attach(std::move(pipe.client), patientOptions());
  REQUIRE_OK(client);
  client->cancel();
  CHECK(!client->isConnected());
  CHECK_ERR(client->call(jt::intCall()), ErrorCode::Cancelled);
  console.finish();
  CHECK(console.lines().empty());
}

TEST(JrpcClientCancel, ReconnectAfterACancelWorks) {
  std::vector<std::unique_ptr<PipeConsole>> consoles;
  std::vector<const ut::MemoryTransport *> probes;
  auto connector = [&]() -> Result<net::TransportPtr> {
    auto pipe = ut::MemoryPipe::create();
    probes.push_back(pipe.client.get());
    const bool first = consoles.empty();
    consoles.push_back(std::make_unique<PipeConsole>(
        std::move(pipe.server), [first](PipeConsole &c) {
          if (first) return hearAndStay(c);
          if (c.readLine()) c.send("2A\r\n");
          c.waitForClose();
        }));
    return net::TransportPtr(std::move(pipe.client));
  };
  auto client = JrpcClient::open(connector, patientOptions());
  REQUIRE_OK(client);

  std::atomic<bool> sawBlocked{false};
  auto canceller = whenBlocked(probes[0], 2, [&] { client->cancel(); }, sawBlocked);
  CHECK_ERR(client->call(jt::intCall()), ErrorCode::Cancelled);
  canceller.join();

  REQUIRE_OK(client->reconnect());
  CHECK(client->isConnected());
  auto r = client->call(jt::intCall());
  REQUIRE_OK(r);
  CHECK(r->value == CallValue(uint64_t{42}));
  CHECK_EQ(consoles.size(), size_t{2});
}

TEST(JrpcClientCancel, AStopRequestEndsTheBannerWait) {
  auto pipe = ut::MemoryPipe::create();
  const auto *probe = pipe.client.get();
  // Accepted but never greeted, like a 9th connection waiting for a slot.
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) { c.waitForClose(); }, false);
  std::stop_source source;
  std::atomic<bool> sawBlocked{false};
  auto canceller = whenBlocked(probe, 2, [&] { source.request_stop(); }, sawBlocked);
  const auto start = Clock::now();
  auto transport = std::make_shared<net::TransportPtr>(std::move(pipe.client));
  auto client = JrpcClient::open([transport]() -> Result<net::TransportPtr> { return std::move(*transport); },
                                 patientOptions(), source.get_token());
  canceller.join();
  CHECK(sawBlocked);
  REQUIRE_ERR(client, ErrorCode::Cancelled);
  CHECK(msSince(start) < 5000);
  console.finish();
  CHECK(console.lines().empty());
}

TEST(JrpcClientCancel, CancelEndsAReconnectWhoseConnectorIsStillRunning) {
  auto first = jt::FakeConsole::create();
  Gate inConnector;
  Gate release;
  auto client = jt::connected(first, jt::quickOptions(), [&]() -> Result<net::TransportPtr> {
    inConnector.open();
    release.wait();
    return jt::FakeConsole::create()->transport();
  });
  Result<void> reconnected;
  std::thread worker([&] { reconnected = client.reconnect(); });
  REQUIRE(inConnector.wait());
  client.cancel();
  release.open();
  worker.join();
  REQUIRE_ERR(reconnected, ErrorCode::Cancelled);
  CHECK(!client.isConnected());
  CHECK_ERR(client.call(jt::intCall()), ErrorCode::Cancelled);
}

// --- A second call, and close() from another thread ---------------------------------

TEST(JrpcClientConcurrent, ASecondCallIsRefusedAndTheFirstIsUntouched) {
  Gate gotCommand;
  Gate release;
  auto pipe = ut::MemoryPipe::create();
  PipeConsole console(std::move(pipe.server), [&](PipeConsole &c) {
    if (!c.readLine()) return;
    gotCommand.open();
    release.wait();
    c.send("2A\r\n");
    if (c.readLine()) c.send("2B\r\n");
    c.waitForClose();
  });
  auto client = JrpcClient::attach(std::move(pipe.client), patientOptions());
  REQUIRE_OK(client);

  Result<CallResult> first = fail(ErrorCode::Unknown, "not run");
  std::thread worker([&] { first = client->call(jt::intCall()); });
  REQUIRE(gotCommand.wait());

  const auto start = Clock::now();
  auto second = client->call(jt::intCall());
  REQUIRE_ERR(second, ErrorCode::InvalidArgument);
  CHECK(second.error().message.find("another call is in progress") != std::string::npos);
  CHECK_ERR(client->rawCommand("x"), ErrorCode::InvalidArgument);
  CHECK_ERR(client->reconnect(), ErrorCode::InvalidArgument);
  CHECK(msSince(start) < 2000);
  CHECK(client->isConnected());

  release.open();
  worker.join();
  REQUIRE_OK(first);
  CHECK(first->value == CallValue(uint64_t{42}));
  CHECK(client->isConnected());
  // The refused calls sent nothing; the connection serves the next one.
  auto third = client->call(jt::intCall());
  REQUIRE_OK(third);
  CHECK(third->value == CallValue(uint64_t{43}));
  client->close();
  console.finish();
  CHECK_EQ(console.lines().size(), size_t{3});
  CHECK(hasLine(console.lines(), "Bye"));
}

TEST(JrpcClientConcurrent, CloseFromAnotherThreadCancelsTheCall) {
  auto pipe = ut::MemoryPipe::create();
  const auto *probe = pipe.client.get();
  PipeConsole console(std::move(pipe.server), hearAndStay);
  auto client = JrpcClient::attach(std::move(pipe.client), patientOptions());
  REQUIRE_OK(client);

  std::atomic<bool> sawBlocked{false};
  auto closer = whenBlocked(probe, 2, [&] { client->close(); }, sawBlocked);
  const auto start = Clock::now();
  auto r = client->call(jt::intCall());
  closer.join();
  CHECK(sawBlocked);
  REQUIRE_ERR(r, ErrorCode::Cancelled);
  CHECK(msSince(start) < 5000);
  CHECK(!client->isConnected());
  console.finish();
  // Writing Bye in the middle of a call could corrupt it: none is written.
  CHECK(!hasLine(console.lines(), "Bye"));
}

TEST(JrpcClientConcurrent, IsConnectedMayBeAskedWhileACallRuns) {
  Gate gotCommand;
  Gate release;
  auto pipe = ut::MemoryPipe::create();
  PipeConsole console(std::move(pipe.server), [&](PipeConsole &c) {
    if (!c.readLine()) return;
    gotCommand.open();
    release.wait();
    c.send("2A\r\n");
    c.waitForClose();
  });
  auto client = JrpcClient::attach(std::move(pipe.client), patientOptions());
  REQUIRE_OK(client);
  Result<CallResult> r = fail(ErrorCode::Unknown, "not run");
  std::thread worker([&] { r = client->call(jt::intCall()); });
  REQUIRE(gotCommand.wait());
  for (int i = 0; i < 100; ++i) CHECK(client->isConnected());
  release.open();
  worker.join();
  CHECK_OK(r);
}

// --- Real timeouts -----------------------------------------------------------------

TEST(JrpcClientRealTime, TheCallTimeoutClosesTheConnection) {
  auto pipe = ut::MemoryPipe::create();
  PipeConsole console(std::move(pipe.server), hearAndStay);
  ClientOptions options = patientOptions();
  options.callTimeout = 150ms;
  options.idleTimeout = 20s;
  auto client = JrpcClient::attach(std::move(pipe.client), options);
  REQUIRE_OK(client);
  const auto start = Clock::now();
  auto r = client->call(jt::intCall());
  const auto elapsed = msSince(start);
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(r.error().message.find("did not complete within 150 ms") != std::string::npos);
  CHECK(elapsed >= 100);
  CHECK(elapsed < 5000);
  CHECK(!client->isConnected());
  // The console sees the connection close, and no Bye.
  console.finish();
  CHECK(!hasLine(console.lines(), "Bye"));
  CHECK_ERR(client->call(jt::intCall()), ErrorCode::NotConnected);
}

TEST(JrpcClientRealTime, ASlowCallWithinTheCallTimeoutSucceeds) {
  // The wait for the first byte is the call timeout, not the idle timeout.
  auto pipe = ut::MemoryPipe::create();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    if (!c.readLine()) return;
    std::this_thread::sleep_for(250ms);
    c.send("2A\r\n");
    c.waitForClose();
  });
  ClientOptions options = patientOptions();
  options.idleTimeout = 50ms;
  options.callTimeout = 10s;
  auto client = JrpcClient::attach(std::move(pipe.client), options);
  REQUIRE_OK(client);
  auto r = client->call(jt::intCall());
  REQUIRE_OK(r);
  CHECK(r->value == CallValue(uint64_t{42}));
  CHECK(client->isConnected());
}

TEST(JrpcClientRealTime, AGapInsideTheReplyIsBoundedByTheIdleTimeout) {
  auto pipe = ut::MemoryPipe::create();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    if (!c.readLine()) return;
    c.send("2");
    c.waitForClose();
  });
  ClientOptions options = patientOptions();
  options.idleTimeout = 100ms;
  options.callTimeout = 20s;
  auto client = JrpcClient::attach(std::move(pipe.client), options);
  REQUIRE_OK(client);
  const auto start = Clock::now();
  auto r = client->call(jt::intCall());
  const auto elapsed = msSince(start);
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(r.error().message.find("nothing for 100 ms") != std::string::npos);
  CHECK(elapsed >= 80);
  CHECK(elapsed < 5000);
  CHECK(!client->isConnected());
}

TEST(JrpcClientRealTime, ASlowTrickleStillMeetsTheCallTimeout) {
  // A byte every 20 ms never trips the idle timeout; the whole call is bounded anyway.
  auto pipe = ut::MemoryPipe::create();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    if (!c.readLine()) return;
    for (int i = 0; i < 400 && !c.stop; ++i) {
      if (!c.send("x")) return;
      std::this_thread::sleep_for(20ms);
    }
  });
  ClientOptions options = patientOptions();
  options.idleTimeout = 2s;
  options.callTimeout = 300ms;
  auto client = JrpcClient::attach(std::move(pipe.client), options);
  REQUIRE_OK(client);
  const auto start = Clock::now();
  auto r = client->call(jt::callAt(0x82000000, ReturnKind::String));
  const auto elapsed = msSince(start);
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(r.error().message.find("did not complete within 300 ms") != std::string::npos);
  CHECK(elapsed >= 250);
  CHECK(elapsed < 4000);
  CHECK(!client->isConnected());
}

TEST(JrpcClientRealTime, TheBannerTimeoutBoundsTheWholeBanner) {
  auto pipe = ut::MemoryPipe::create();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    for (const char *piece : {"JRPC2 ", "conn", "ect", "ed"}) {
      if (!c.send(piece)) return;
      std::this_thread::sleep_for(60ms);
    }
    c.waitForClose();
  }, false);
  ClientOptions options = patientOptions();
  options.bannerTimeout = 150ms;
  const auto start = Clock::now();
  auto client = JrpcClient::attach(std::move(pipe.client), options);
  const auto elapsed = msSince(start);
  REQUIRE_ERR(client, ErrorCode::Timeout);
  CHECK(client.error().message.find("did not arrive within 150 ms") != std::string::npos);
  CHECK(elapsed < 4000);
}
