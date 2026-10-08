#pragma once

#include "support/jrpc_mock_server.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <net/endpoint.hpp>
#include <net/tcp_transport.hpp>
#include <protocols/jrpc/client.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The client against the mock server, where the two independently written halves of
// docs/JRPC_PROTOCOL.md meet. Every test runs twice: over an in-memory pipe and over
// loopback TCP (skipped when the sandbox refuses sockets).
namespace jit {

using namespace std::chrono_literals;
using updclient::ErrorCode;
using updclient::Result;
using updclient::jrpc::Arg;
using updclient::jrpc::CallSpec;
using updclient::jrpc::CallValue;
using updclient::jrpc::ClientOptions;
using updclient::jrpc::JrpcClient;
using updclient::jrpc::ReturnKind;
using ut::Bytes;
using ut::JrpcCall;
using ut::JrpcFault;
using ut::JrpcMockOptions;
using ut::JrpcMockServer;
using ut::JrpcReturn;

enum class Link { Memory, Tcp };

// Generous timeouts for the tests that expect everything to work, so that a loaded
// machine does not fail them; a test about a timeout sets its own short ones.
inline ClientOptions quickOptions() {
  ClientOptions o;
  o.bannerTimeout = 3000ms;
  o.idleTimeout = 3000ms;
  o.callTimeout = 20000ms;
  o.byeTimeout = 1000ms;
  return o;
}

// Short ones, for a console that is expected to go quiet.
inline ClientOptions impatient() {
  ClientOptions o = quickOptions();
  o.bannerTimeout = 300ms;
  o.idleTimeout = 300ms;
  o.callTimeout = 600ms;
  return o;
}

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
  bool isOpen() {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool open_ = false;
};

// Opens a gate when it goes out of scope. Declared after the Rig, it runs before the mock
// is stopped, so that a registered function blocked on the gate cannot keep the mock's
// connection thread (and with it the destructor) waiting.
struct Release {
  explicit Release(std::shared_ptr<Gate> g) : gate(std::move(g)) {}
  ~Release() { gate->open(); }
  std::shared_ptr<Gate> gate;
};

class Rig {
public:
  explicit Rig(Link link, JrpcMockOptions options = {}) : mock(options), link_(link) {
    if (link_ == Link::Tcp) {
      auto port = mock.listenTcp();
      if (!port) ut::skip("loopback TCP refused by the environment: " + updclient::formatError(port.error()));
      port_ = *port;
    }
  }

  JrpcMockServer mock;

  Link link() const { return link_; }
  uint16_t port() const { return port_; }

  updclient::net::Endpoint endpoint() const {
    updclient::net::Endpoint e;
    e.scheme = "jrpc";
    e.host = "127.0.0.1";
    e.port = port_;
    e.timeout = 5000ms;
    return e;
  }

  JrpcClient::Connector connector() {
    if (link_ == Link::Memory) return mock.connector();
    const uint16_t port = port_;
    return [port] { return updclient::net::TcpTransport::connect("127.0.0.1", port, 5000ms); };
  }

  // Over TCP through JrpcClient::connect and the jrpc:// endpoint, as an application
  // would; in memory through open().
  Result<JrpcClient> open(ClientOptions options = quickOptions()) {
    if (link_ == Link::Tcp) return JrpcClient::connect(endpoint(), options);
    return JrpcClient::open(connector(), options);
  }

  // A client; a failure to connect fails the test and ends it.
  JrpcClient client(ClientOptions options = quickOptions()) {
    auto c = open(options);
    if (!c) {
      ut::recordFailure(__FILE__, __LINE__, "opening a client", updclient::formatError(c.error()));
      throw ut::AbortSignal{};
    }
    return std::move(*c);
  }

  // As open() and client(), once the mock has noticed that every earlier connection
  // closed: a test that opens one client after another would otherwise race the mock's
  // bookkeeping into its connection limit.
  Result<JrpcClient> openFresh(ClientOptions options = quickOptions()) {
    (void)mock.waitForActiveConnections(0);
    return open(options);
  }
  JrpcClient fresh(ClientOptions options = quickOptions()) {
    (void)mock.waitForActiveConnections(0);
    return client(options);
  }

  // The client sends one command and waits for its answer (D3). The exceptions are the
  // opcodes that may not answer (notify, setLeds, constantMemorySet): with the barrier of
  // D6 the client writes ConsoleType right behind them, and without it the next command
  // may follow at once, so the mock can find more bytes waiting when it takes such a
  // line. Nothing else may be pipelined, an empty line is never sent, and no line is
  // over-long or unrecognised.
  void checkCleanTraffic() const {
    for (const auto &record : mock.commands()) {
      if (record.pipelined) {
        CHECK_MSG(record.name == "notify" || record.name == "leds" || record.name == "constmem" ||
                      record.name == "shutdown",
                  "pipelined: " + record.line.substr(0, 80));
      }
      CHECK_MSG(record.name != "empty" && record.name != "invalid" && record.name != "overlong",
                "a line the console cannot use: " + record.line.substr(0, 80));
      CHECK_MSG(!record.overLong, "over-long line: " + record.line.substr(0, 60));
    }
  }

  std::vector<ut::JrpcCommandRecord> recordsNamed(std::string_view name) const {
    std::vector<ut::JrpcCommandRecord> out;
    for (const auto &record : mock.commands()) {
      if (record.name == name) out.push_back(record);
    }
    return out;
  }

private:
  Link link_;
  uint16_t port_ = 0;
};

// The value of a result that is checked to have one; a default value if it has none, so
// that a failure is reported instead of aborting the run.
template <class T> T got(const Result<T> &result) {
  CHECK_OK(result);
  return result ? *result : T{};
}

inline CallSpec callAt(uint32_t address, ReturnKind returns = ReturnKind::Void, std::vector<Arg> args = {},
                       size_t arraySize = 0) {
  CallSpec spec;
  spec.target = address;
  spec.returns = returns;
  spec.arraySize = arraySize;
  spec.args = std::move(args);
  return spec;
}

inline const char *linkName(Link link) { return link == Link::Memory ? "memory" : "tcp"; }

inline long long msSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

} // namespace jit

// A test body run once per link: TEST(Suite, NameMemory) and TEST(Suite, NameTcp).
#define JRPC_LINK_TEST(suite, name)                                                                                 \
  static void suite##_##name##_body(jit::Link link);                                                                \
  TEST(suite, name##Memory) { suite##_##name##_body(jit::Link::Memory); }                                           \
  TEST(suite, name##Tcp) { suite##_##name##_body(jit::Link::Tcp); }                                                 \
  static void suite##_##name##_body(jit::Link link)

// The same for a known disagreement between client and mock, until it is fixed.
#define JRPC_LINK_XFAIL_TEST(suite, name, reason)                                                                   \
  static void suite##_##name##_body(jit::Link link);                                                                \
  XFAIL_TEST(suite, name##Memory, reason) { suite##_##name##_body(jit::Link::Memory); }                             \
  XFAIL_TEST(suite, name##Tcp, reason) { suite##_##name##_body(jit::Link::Tcp); }                                   \
  static void suite##_##name##_body(jit::Link link)
