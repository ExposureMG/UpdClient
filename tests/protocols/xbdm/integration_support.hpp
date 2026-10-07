#pragma once

#include "support/test_harness.hpp"
#include "support/test_util.hpp"
#include "support/xbdm_mock_server.hpp"

#include <net/endpoint.hpp>
#include <net/tcp_transport.hpp>
#include <protocols/xbdm/client.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The client against the mock server, the first time the two independently
// written halves of docs/XBDM_PROTOCOL.md meet. Every test runs twice: over an
// in-memory pipe and over loopback TCP (skipped when the sandbox refuses sockets).
namespace xit {

using namespace std::chrono_literals;
using updclient::ErrorCode;
using updclient::Result;
using updclient::xbdm::ClientOptions;
using updclient::xbdm::XbdmClient;
using ut::Bytes;
using ut::XbdmFault;
using ut::XbdmMockOptions;
using ut::XbdmMockServer;

enum class Link { Memory, Tcp };

// Short timeouts, so a test that waits for one does not take long.
inline ClientOptions quickOptions() {
  ClientOptions o;
  o.greetingTimeout = 3000ms;
  o.idleTimeout = 3000ms;
  o.slowIdleTimeout = 3000ms;
  o.commandTimeout = 20000ms;
  o.byeTimeout = 1000ms;
  return o;
}

class Rig {
public:
  explicit Rig(Link link, XbdmMockOptions options = {}) : mock(options), link_(link) {
    if (link_ == Link::Tcp) {
      auto port = mock.listenTcp();
      if (!port) ut::skip("loopback TCP refused by the environment: " + updclient::formatError(port.error()));
      port_ = *port;
    }
  }

  XbdmMockServer mock;

  Link link() const { return link_; }
  uint16_t port() const { return port_; }

  updclient::net::Endpoint endpoint() const {
    updclient::net::Endpoint e;
    e.scheme = "xbdm";
    e.host = "127.0.0.1";
    e.port = port_;
    e.timeout = 5000ms;
    return e;
  }

  XbdmClient::Connector connector() {
    if (link_ == Link::Memory) return mock.connector();
    const uint16_t port = port_;
    return [port] { return updclient::net::TcpTransport::connect("127.0.0.1", port, 5000ms); };
  }

  // Over TCP through XbdmClient::connect and the xbdm:// endpoint, as an
  // application would; in memory through open().
  Result<XbdmClient> open(ClientOptions options = quickOptions()) {
    if (link_ == Link::Tcp) return XbdmClient::connect(endpoint(), options);
    return XbdmClient::open(connector(), options);
  }

  XbdmClient client(ClientOptions options = quickOptions()) {
    auto c = open(options);
    if (!c) {
      ut::recordFailure(__FILE__, __LINE__, "opening a client", updclient::formatError(c.error()));
      throw ut::AbortSignal{};
    }
    return std::move(*c);
  }

  // As open() and client(), once the mock has noticed that every earlier connection
  // closed: a test that opens one client after another would otherwise race the
  // mock's bookkeeping into its connection limit.
  Result<XbdmClient> openFresh(ClientOptions options = quickOptions()) {
    (void)mock.waitForActiveConnections(0);
    return open(options);
  }
  XbdmClient fresh(ClientOptions options = quickOptions()) {
    (void)mock.waitForActiveConnections(0);
    return client(options);
  }

  // The client never pipelines and never sends an empty line (sections 1.3, 1.10).
  void checkCleanTraffic() const {
    for (const auto &record : mock.commands()) {
      CHECK_MSG(!record.pipelined, "pipelined: " + record.line);
      CHECK_MSG(!record.name.empty(), "an empty command line was sent");
      CHECK_MSG(!record.overLong, "over-long line: " + record.line.substr(0, 60));
    }
  }

  std::vector<std::string> linesNamed(std::string_view name) const {
    std::vector<std::string> out;
    for (const auto &record : mock.commands()) {
      if (record.name == name) out.push_back(record.line);
    }
    return out;
  }

private:
  Link link_;
  uint16_t port_ = 0;
};

template <class T> Result<void> asVoid(const Result<T> &result) {
  if (!result) return updclient::unexpected<updclient::Error>(result.error());
  return {};
}

inline const char *linkName(Link link) { return link == Link::Memory ? "memory" : "tcp"; }

// Names under a folder that look like the client's temporary upload names.
inline std::vector<std::string> partFiles(const XbdmMockServer &mock, std::string_view folder) {
  std::vector<std::string> out;
  for (const auto &name : mock.listNames(folder).value_or(std::vector<std::string>{})) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0) out.push_back(name);
  }
  return out;
}

} // namespace xit

// A test body run once per link: TEST(Suite, NameMemory) and TEST(Suite, NameTcp).
#define XBDM_LINK_TEST(suite, name)                                                                                 \
  static void suite##_##name##_body(xit::Link link);                                                                \
  TEST(suite, name##Memory) { suite##_##name##_body(xit::Link::Memory); }                                           \
  TEST(suite, name##Tcp) { suite##_##name##_body(xit::Link::Tcp); }                                                 \
  static void suite##_##name##_body(xit::Link link)

// The same for a known disagreement between client and mock, until it is fixed.
#define XBDM_LINK_XFAIL_TEST(suite, name, reason)                                                                   \
  static void suite##_##name##_body(xit::Link link);                                                                \
  XFAIL_TEST(suite, name##Memory, reason) { suite##_##name##_body(xit::Link::Memory); }                             \
  XFAIL_TEST(suite, name##Tcp, reason) { suite##_##name##_body(xit::Link::Tcp); }                                   \
  static void suite##_##name##_body(xit::Link link)
