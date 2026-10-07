#include "support/memory_transport.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/xbdm/client.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// close() from another thread, and real timeouts, over the blocking in-memory
// transport: the console runs on its own thread, so every wait is a real one.

using namespace updclient;
using namespace updclient::xbdm;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

// The console end of the pipe, driven by a script on its own thread.
class PipeConsole {
public:
  using Script = std::function<void(PipeConsole &)>;

  PipeConsole(std::unique_ptr<ut::MemoryTransport> end, Script script) : end_(std::move(end)) {
    thread_ = std::thread([this, script = std::move(script)] {
      if (send("201- connected\r\n")) script(*this);
    });
  }
  ~PipeConsole() {
    stop = true;
    end_->close();
    if (thread_.joinable()) thread_.join();
  }

  bool send(std::string_view text) { return static_cast<bool>(end_->writeAll(ut::bytesOf(text))); }
  bool send(const ut::Bytes &bytes) { return static_cast<bool>(end_->writeAll(bytes)); }

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
    return line;
  }

  bool readBytes(size_t count) {
    std::vector<uint8_t> buffer(std::min<size_t>(count, 65536));
    while (count > 0) {
      auto n = end_->readSome(std::span<uint8_t>(buffer.data(), std::min(count, buffer.size())));
      if (!n || *n == 0) return false;
      count -= *n;
    }
    return true;
  }

  // Blocks until the client closes its end.
  void waitForClose() {
    uint8_t c = 0;
    while (!stop) {
      auto n = end_->readSome(std::span<uint8_t>(&c, 1));
      if (!n || *n == 0) return;
    }
  }

  std::atomic<bool> stop{false};

private:
  std::unique_ptr<ut::MemoryTransport> end_;
  std::thread thread_;
};

ClientOptions patientOptions() {
  ClientOptions options;
  options.greetingTimeout = 20s;
  options.idleTimeout = 20s;
  options.slowIdleTimeout = 20s;
  options.commandTimeout = 0ms;
  options.byeTimeout = 50ms;
  return options;
}

ut::Bytes le32(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

// Calls cancel once `blocked` calls on the pipe are waiting.
std::thread cancelWhenBlocked(const ut::MemoryTransport *probe, size_t blocked, std::function<void()> cancel,
                              std::atomic<bool> &sawBlocked) {
  return std::thread([probe, blocked, cancel = std::move(cancel), &sawBlocked] {
    sawBlocked = probe->waitUntilBlocked(blocked, 10s);
    cancel();
  });
}

long long msSince(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

} // namespace

TEST(XbdmCancel, CancelEndsADownloadReadThatIsWaiting) {
  auto pipe = ut::MemoryPipe::create();
  const auto *probe = pipe.client.get();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    if (!c.readLine()) return;
    c.send(std::string_view("203- binary response follows\r\n"));
    c.send(le32(1u << 20));
    c.send(ut::Bytes(1000, 7));
    c.waitForClose();
  });
  auto client = XbdmClient::attach(std::move(pipe.client), patientOptions());
  REQUIRE_OK(client);
  auto reader = client->openRead("HDD:\\big.bin");
  REQUIRE_OK(reader);
  std::vector<uint8_t> buffer(4096);
  size_t got = 0;
  while (got < 1000) {
    auto n = reader->read(buffer);
    REQUIRE_OK(n);
    got += *n;
  }

  std::atomic<bool> sawBlocked{false};
  auto canceller = cancelWhenBlocked(probe, 2, [&] { reader->cancel(); }, sawBlocked);
  const auto start = Clock::now();
  auto n = reader->read(buffer);
  canceller.join();
  CHECK(sawBlocked);
  REQUIRE_ERR(n, ErrorCode::Cancelled);
  CHECK(msSince(start) < 5000);
  CHECK(!reader->isOpen());
  CHECK(!client->isConnected());
  CHECK(!client->transferActive());
  CHECK_ERR(client->debugName(), ErrorCode::Cancelled);
}

TEST(XbdmCancel, CancelEndsACommandWaitingForItsAnswer) {
  auto pipe = ut::MemoryPipe::create();
  const auto *probe = pipe.client.get();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    if (!c.readLine()) return;
    c.waitForClose();
  });
  auto client = XbdmClient::attach(std::move(pipe.client), patientOptions());
  REQUIRE_OK(client);
  std::atomic<bool> sawBlocked{false};
  auto canceller = cancelWhenBlocked(probe, 2, [&] { client->cancel(); }, sawBlocked);
  const auto start = Clock::now();
  auto name = client->debugName();
  canceller.join();
  CHECK(sawBlocked);
  REQUIRE_ERR(name, ErrorCode::Cancelled);
  CHECK(msSince(start) < 5000);
  CHECK(!client->isConnected());
}

TEST(XbdmCancel, CancelEndsAnUploadWriteThatIsWaiting) {
  auto pipe = ut::MemoryPipe::create(4096);
  const auto *probe = pipe.client.get();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    if (!c.readLine()) return;
    c.send(std::string_view("204- send binary data\r\n"));
    while (!c.stop) std::this_thread::sleep_for(1ms);
  });
  auto client = XbdmClient::attach(std::move(pipe.client), patientOptions());
  REQUIRE_OK(client);
  const ut::Bytes data(1u << 20, 0x5A);
  auto writer = client->openWrite("HDD:\\up.bin", data.size());
  REQUIRE_OK(writer);
  const std::string temp = writer->temporaryPath();

  std::atomic<bool> sawBlocked{false};
  auto canceller = cancelWhenBlocked(probe, 1, [&] { writer->cancel(); }, sawBlocked);
  const auto start = Clock::now();
  auto r = writer->write(data);
  canceller.join();
  console.stop = true;
  CHECK(sawBlocked);
  REQUIRE_ERR(r, ErrorCode::Cancelled);
  CHECK(msSince(start) < 5000);
  CHECK(!writer->isOpen());
  CHECK(!client->isConnected());
  CHECK_EQ(client->pendingCleanup(), std::vector<std::string>{temp});
}

TEST(XbdmCancel, CancelEndsAFinishWaitingForTheConsole) {
  auto pipe = ut::MemoryPipe::create();
  const auto *probe = pipe.client.get();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    if (!c.readLine()) return;
    c.send(std::string_view("204- send binary data\r\n"));
    if (!c.readBytes(5000)) return;
    c.waitForClose();
  });
  auto client = XbdmClient::attach(std::move(pipe.client), patientOptions());
  REQUIRE_OK(client);
  auto writer = client->openWrite("HDD:\\up.bin", 5000);
  REQUIRE_OK(writer);
  REQUIRE_OK(writer->write(ut::Bytes(5000, 1)));
  std::atomic<bool> sawBlocked{false};
  auto canceller = cancelWhenBlocked(probe, 2, [&] { writer->cancel(); }, sawBlocked);
  auto r = writer->finish();
  canceller.join();
  CHECK(sawBlocked);
  REQUIRE_ERR(r, ErrorCode::Cancelled);
  CHECK_EQ(client->pendingCleanup().size(), size_t{1});
}

TEST(XbdmCancel, IdleTimeoutIsReal) {
  auto pipe = ut::MemoryPipe::create();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    if (!c.readLine()) return;
    c.waitForClose();
  });
  ClientOptions options = patientOptions();
  options.idleTimeout = 100ms;
  auto client = XbdmClient::attach(std::move(pipe.client), options);
  REQUIRE_OK(client);
  const auto start = Clock::now();
  auto r = client->debugName();
  const auto elapsed = msSince(start);
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(r.error().message.find("stopped responding") != std::string::npos);
  CHECK(elapsed >= 90);
  CHECK(elapsed < 3000);
  CHECK(!client->isConnected());
}

TEST(XbdmCancel, GreetingTimeoutIsReal) {
  auto pipe = ut::MemoryPipe::create();
  auto server = std::move(pipe.server);
  ClientOptions options = patientOptions();
  options.greetingTimeout = 80ms;
  const auto start = Clock::now();
  CHECK_ERR(XbdmClient::attach(std::move(pipe.client), options), ErrorCode::Timeout);
  CHECK(msSince(start) < 3000);
}

TEST(XbdmCancel, CommandDeadlineBoundsATricklingAnswer) {
  auto pipe = ut::MemoryPipe::create();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    if (!c.readLine()) return;
    while (!c.stop && c.send(std::string_view("x"))) std::this_thread::sleep_for(10ms);
  });
  ClientOptions options = patientOptions();
  options.idleTimeout = 1000ms;
  options.commandTimeout = 150ms;
  options.maxLineBytes = 1u << 20;
  auto client = XbdmClient::attach(std::move(pipe.client), options);
  REQUIRE_OK(client);
  const auto start = Clock::now();
  auto r = client->debugName();
  const auto elapsed = msSince(start);
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK_MSG(r.error().message.find("within 150 ms") != std::string::npos, r.error().message);
  CHECK(elapsed >= 140);
  CHECK(elapsed < 3000);
}

// Without a bound on the whole line, one byte just inside the idle timeout would
// hold each of these for up to maxLineBytes idle timeouts.
TEST(XbdmCancel, DeadlinesBoundATricklingGreeting) {
  auto pipe = ut::MemoryPipe::create();
  std::atomic<bool> stop{false};
  std::thread console([&, server = pipe.server.get()] {
    if (!server->writeAll(ut::bytesOf("201- "))) return;
    while (!stop && server->writeAll(ut::bytesOf("x"))) std::this_thread::sleep_for(5ms);
  });
  ClientOptions options = patientOptions();
  options.greetingTimeout = 150ms;
  const auto start = Clock::now();
  auto client = XbdmClient::attach(std::move(pipe.client), options);
  const auto elapsed = msSince(start);
  stop = true;
  pipe.server->close();
  console.join();
  REQUIRE_ERR(client, ErrorCode::Timeout);
  CHECK_MSG(client.error().message.find("greeting did not arrive within 150 ms") != std::string::npos,
            client.error().message);
  CHECK(elapsed >= 140);
  CHECK(elapsed < 2000);
}

TEST(XbdmCancel, DeadlinesBoundATricklingAnswerToBye) {
  auto pipe = ut::MemoryPipe::create();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    if (c.readLine() != "bye") return;
    if (!c.send(std::string_view("200- "))) return;
    while (!c.stop && c.send(std::string_view("x"))) std::this_thread::sleep_for(5ms);
  });
  ClientOptions options = patientOptions();
  options.byeTimeout = 150ms;
  auto client = XbdmClient::attach(std::move(pipe.client), options);
  REQUIRE_OK(client);
  const auto start = Clock::now();
  client->close();
  const auto elapsed = msSince(start);
  CHECK(elapsed >= 140);
  CHECK(elapsed < 2000);
  CHECK(!client->isConnected());
}

TEST(XbdmCancel, DeadlinesBoundATricklingStatusAfterSendfileData) {
  auto pipe = ut::MemoryPipe::create();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    if (!c.readLine()) return;
    if (!c.send(std::string_view("204- send binary data\r\n")) || !c.readBytes(4)) return;
    if (!c.send(std::string_view("200- "))) return;
    while (!c.stop && c.send(std::string_view("x"))) std::this_thread::sleep_for(5ms);
  });
  ClientOptions options = patientOptions();
  options.slowIdleTimeout = 1000ms;
  options.commandTimeout = 150ms;
  auto client = XbdmClient::attach(std::move(pipe.client), options);
  REQUIRE_OK(client);
  auto writer = client->openWrite("HDD:\\a.bin", 4);
  REQUIRE_OK(writer);
  REQUIRE_OK(writer->write(ut::Bytes{1, 2, 3, 4}));
  const auto start = Clock::now();
  auto r = writer->finish();
  const auto elapsed = msSince(start);
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK_MSG(r.error().message.find("within 150 ms") != std::string::npos, r.error().message);
  CHECK(elapsed >= 140);
  CHECK(elapsed < 2000);
}

TEST(XbdmCancel, CancelDuringReconnectIsNotLost) {
  std::vector<std::unique_ptr<PipeConsole>> consoles;
  std::atomic<bool> connecting{false};
  std::atomic<int> connects{0};
  auto connector = [&]() -> Result<net::TransportPtr> {
    auto pipe = ut::MemoryPipe::create();
    consoles.push_back(std::make_unique<PipeConsole>(std::move(pipe.server), [](PipeConsole &c) {
      while (auto line = c.readLine()) {
        if (*line == "bye") return;
        if (!c.send(std::string_view("200- name\r\n"))) return;
      }
    }));
    if (connects++ == 1) {
      connecting = true;
      std::this_thread::sleep_for(300ms);
    }
    return net::TransportPtr(std::move(pipe.client));
  };
  auto client = XbdmClient::open(connector, patientOptions());
  REQUIRE_OK(client);
  std::thread canceller([&] {
    while (!connecting) std::this_thread::yield();
    std::this_thread::sleep_for(50ms);
    client->cancel();
  });
  auto r = client->reconnect();
  canceller.join();
  CHECK_ERR(r, ErrorCode::Cancelled);
  CHECK(!client->isConnected());
  CHECK_ERR(client->debugName(), ErrorCode::Cancelled);
  // A cancel before reconnect() began does not count against it.
  REQUIRE_OK(client->reconnect());
  CHECK_EQ(client->debugName().value_or(""), std::string("name"));
}

TEST(XbdmCancel, TransferBodiesAreExemptFromTheCommandDeadline) {
  auto pipe = ut::MemoryPipe::create();
  const ut::Bytes file = ut::patternBytes(2000, 21);
  PipeConsole console(std::move(pipe.server), [&](PipeConsole &c) {
    if (!c.readLine()) return;
    c.send(std::string_view("203- binary response follows\r\n"));
    c.send(le32(2000));
    for (size_t offset = 0; offset < file.size(); offset += 100) {
      std::this_thread::sleep_for(15ms);
      if (!c.send(ut::Bytes(file.begin() + static_cast<std::ptrdiff_t>(offset),
                            file.begin() + static_cast<std::ptrdiff_t>(offset + 100)))) {
        return;
      }
    }
    c.waitForClose();
  });
  ClientOptions options = patientOptions();
  options.idleTimeout = 2000ms;
  options.commandTimeout = 100ms;
  auto client = XbdmClient::attach(std::move(pipe.client), options);
  REQUIRE_OK(client);
  auto reader = client->openRead("HDD:\\slow.bin");
  REQUIRE_OK(reader);
  ut::Bytes got;
  std::vector<uint8_t> buffer(512);
  const auto start = Clock::now();
  for (;;) {
    auto n = reader->read(buffer);
    REQUIRE_OK(n);
    if (*n == 0) break;
    got.insert(got.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*n));
  }
  CHECK(msSince(start) >= 100);
  CHECK_EQ(got, file);
  CHECK(client->isConnected());
}

TEST(XbdmCancel, CancelFromManyThreadsAtOnce) {
  auto pipe = ut::MemoryPipe::create();
  const auto *probe = pipe.client.get();
  PipeConsole console(std::move(pipe.server), [](PipeConsole &c) {
    if (!c.readLine()) return;
    c.waitForClose();
  });
  auto client = XbdmClient::attach(std::move(pipe.client), patientOptions());
  REQUIRE_OK(client);
  std::atomic<bool> sawBlocked{false};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  threads.emplace_back([&, probe] {
    sawBlocked = probe->waitUntilBlocked(2, 10s);
    go = true;
  });
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&] {
      while (!go) std::this_thread::yield();
      client->cancel();
    });
  }
  auto r = client->drives();
  for (auto &thread : threads) thread.join();
  CHECK(sawBlocked);
  CHECK_ERR(r, ErrorCode::Cancelled);
}
