#include "protocols/jrpc/client_fake.hpp"
#include "support/jrpc_mock_server.hpp"
#include "support/loopback_server.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/jrpc/client.hpp>
#include <protocols/jrpc/discovery.hpp>
#include <updclient.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// The scheme registration, identify() and the probe provider, against the J2 mock, whose
// parser was written from the protocol document and shares no code with the client.

using namespace updclient;
using namespace updclient::jrpc;
using namespace std::chrono_literals;
using ut::JrpcFault;
using ut::JrpcMockOptions;
using ut::JrpcMockServer;

namespace {

ClientOptions quick() {
  ClientOptions o = jt::quickOptions();
  o.bannerTimeout = 300ms;
  return o;
}

net::Endpoint jrpcEndpoint(const std::string &host, uint16_t port = 0, const std::string &scheme = "jrpc") {
  net::Endpoint e;
  e.scheme = scheme;
  e.host = host;
  e.port = port;
  e.timeout = 3000ms;
  return e;
}

// A mock serving loopback TCP, or nothing when the environment refuses sockets.
struct TcpMock {
  JrpcMockServer server;
  uint16_t port = 0;
  std::string skipReason;

  explicit TcpMock(JrpcMockOptions options = {}) : server(std::move(options)) {
    auto p = server.listenTcp();
    if (p) {
      port = *p;
    } else {
      skipReason = "loopback TCP refused: " + formatError(p.error());
    }
  }
};

// Every record the mock took, by name; "bye" is the only one a clean identify() makes.
std::vector<std::string> commandNames(const JrpcMockServer &mock) {
  std::vector<std::string> names;
  for (const auto &c : mock.commands()) names.push_back(c.name);
  return names;
}

} // namespace

// --- registerJrpcScheme ---

TEST(JrpcScheme, RegistersJrpcWithPort1409) {
  net::TransportRegistry registry;
  registerJrpcScheme(registry);
  CHECK_EQ(registry.schemes(), std::vector<std::string>{"jrpc"});
  // jrpc:// gets 1409 whatever protocol port the caller passes, an explicit port stays.
  auto bare = net::Endpoint::parse("jrpc://192.168.1.20");
  REQUIRE_OK(bare);
  CHECK_EQ(registry.withDefaultPort(*bare, 49).port, 1409);
  CHECK_EQ(registry.withDefaultPort(*bare, 0).port, 1409);
  auto explicitPort = net::Endpoint::parse("jrpc://192.168.1.20:1410");
  REQUIRE_OK(explicitPort);
  CHECK_EQ(registry.withDefaultPort(*explicitPort, 49).port, 1410);
  // Schemes are case-insensitive.
  auto upper = net::Endpoint::parse("JRPC://192.168.1.20");
  REQUIRE_OK(upper);
  CHECK_EQ(registry.withDefaultPort(*upper, 49).port, 1409);
}

TEST(JrpcScheme, ABareTcpHostGets1409FromTheProtocolPort) {
  net::TransportRegistry registry;
  registerJrpcScheme(registry);
  net::Endpoint tcp = jrpcEndpoint("192.168.1.20", 0, "tcp");
  // tcp carries the protocol's own port, which JrpcClient passes as kJrpcPort.
  CHECK_EQ(registry.withDefaultPort(tcp, kJrpcPort).port, 1409);
  tcp.port = 2000;
  CHECK_EQ(registry.withDefaultPort(tcp, kJrpcPort).port, 2000);
}

TEST(JrpcScheme, RegistrationLeavesOtherSchemesAlone) {
  net::TransportRegistry registry;
  registerJrpcScheme(registry);
  net::Endpoint other = jrpcEndpoint("host", 0, "serialprobe");
  CHECK_EQ(registry.withDefaultPort(other, 80).port, 0);
  // Registering twice replaces the entry.
  registerJrpcScheme(registry);
  CHECK_EQ(registry.schemes().size(), size_t{1});
  // Without the call the scheme is unknown.
  net::TransportRegistry empty;
  CHECK_ERR(empty.connect(jrpcEndpoint("127.0.0.1", 1, "jrpc")), ErrorCode::Unsupported);
}

TEST(JrpcScheme, TheRegisteredConnectorReachesTheMock) {
  TcpMock mock;
  if (!mock.skipReason.empty()) SKIP(mock.skipReason);
  net::TransportRegistry registry;
  registerJrpcScheme(registry);
  auto transport = registry.connect(jrpcEndpoint("127.0.0.1", mock.port));
  REQUIRE_OK(transport);
  auto client = JrpcClient::attach(std::move(*transport), quick());
  REQUIRE_OK(client);
  CHECK(client->isConnected());
  client->close();
  CHECK(mock.server.waitForActiveConnections(0));
}

TEST(JrpcScheme, TheRegisteredConnectorFailsCleanlyWhenNothingListens) {
  net::TransportRegistry registry;
  registerJrpcScheme(registry);
  const uint16_t port = ut::unusedLoopbackPort();
  if (port == 0) SKIP("no loopback port");
  CHECK_ERR(registry.connect(jrpcEndpoint("127.0.0.1", port)), ErrorCode::ConnectFailed);
}

// --- identify ---

TEST(JrpcIdentify, GoodBannerAnswersWithTheEndpointAndTheBannerAndSendsOnlyBye) {
  TcpMock mock;
  if (!mock.skipReason.empty()) SKIP(mock.skipReason);
  auto result = identify(jrpcEndpoint("127.0.0.1", mock.port), quick());
  REQUIRE_OK(result);
  CHECK_EQ(result->endpoint.host, std::string("127.0.0.1"));
  CHECK_EQ(result->endpoint.port, mock.port);
  CHECK_EQ(result->endpoint.scheme, std::string("jrpc"));
  CHECK_EQ(result->banner, std::string("JRPC2 connected"));
  // Nothing was sent but "Bye", and the connection is gone.
  REQUIRE(mock.server.waitForCommands(1));
  CHECK(mock.server.waitForActiveConnections(0));
  CHECK_EQ(commandNames(mock.server), std::vector<std::string>{"bye"});
  CHECK(mock.server.calls().empty());
  CHECK(mock.server.events().empty());
}

TEST(JrpcIdentify, WrongBannerIsAProtocolError) {
  JrpcMockOptions o;
  o.banner = "220 smtp ready";
  TcpMock mock(o);
  if (!mock.skipReason.empty()) SKIP(mock.skipReason);
  auto r = identify(jrpcEndpoint("127.0.0.1", mock.port), quick());
  REQUIRE_ERR(r, ErrorCode::Protocol);
  CHECK_MSG(r.error().message.find("220 smtp ready") != std::string::npos, r.error().message);
  CHECK(mock.server.waitForActiveConnections(0));
}

TEST(JrpcIdentify, ABannerThatIsOnlyCloseToTheRealOneIsRejected) {
  for (const char *banner : {"JRPC connected", "jrpc2 connected", "JRPC2 connected ", "JRPC2 connected!"}) {
    JrpcMockOptions o;
    o.banner = banner;
    TcpMock mock(o);
    if (!mock.skipReason.empty()) SKIP(mock.skipReason);
    CHECK_ERR(identify(jrpcEndpoint("127.0.0.1", mock.port), quick()), ErrorCode::Protocol);
  }
}

TEST(JrpcIdentify, NoBannerIsATimeout) {
  JrpcMockOptions o;
  o.sendBanner = false;
  TcpMock mock(o);
  if (!mock.skipReason.empty()) SKIP(mock.skipReason);
  const auto start = std::chrono::steady_clock::now();
  auto r = identify(jrpcEndpoint("127.0.0.1", mock.port), quick());
  const auto took = std::chrono::steady_clock::now() - start;
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK_MSG(took < 3s, "the banner wait was not bounded by bannerTimeout");
  CHECK(mock.server.waitForActiveConnections(0));
}

TEST(JrpcIdentify, ADebugLineMeansJrpcIsNotInstalled) {
  TcpMock mock;
  if (!mock.skipReason.empty()) SKIP(mock.skipReason);
  mock.server.inject(JrpcFault::debugLine().onGreeting());
  auto r = identify(jrpcEndpoint("127.0.0.1", mock.port), quick());
  REQUIRE_ERR(r, ErrorCode::Unsupported);
  CHECK_MSG(r.error().message.find("not installed") != std::string::npos, r.error().message);
  CHECK(mock.server.waitForActiveConnections(0));
}

TEST(JrpcIdentify, AConnectionThatClosesWithoutABannerIsAnError) {
  TcpMock mock;
  if (!mock.skipReason.empty()) SKIP(mock.skipReason);
  mock.server.inject(JrpcFault::dropConnection().onGreeting());
  auto r = identify(jrpcEndpoint("127.0.0.1", mock.port), quick());
  CHECK(!r);
}

TEST(JrpcIdentify, NothingListeningIsConnectFailed) {
  const uint16_t port = ut::unusedLoopbackPort();
  if (port == 0) SKIP("no loopback port");
  CHECK_ERR(identify(jrpcEndpoint("127.0.0.1", port), quick()), ErrorCode::ConnectFailed);
}

TEST(JrpcIdentify, AStoppedTokenCancelsAtOnce) {
  std::stop_source source;
  source.request_stop();
  CHECK_ERR(identify(jrpcEndpoint("127.0.0.1", 1), quick(), source.get_token()), ErrorCode::Cancelled);
}

TEST(JrpcIdentify, AStopDuringTheBannerWaitCancels) {
  JrpcMockOptions o;
  o.sendBanner = false;
  TcpMock mock(o);
  if (!mock.skipReason.empty()) SKIP(mock.skipReason);
  ClientOptions options = quick();
  options.bannerTimeout = 20s;
  std::stop_source source;
  std::thread stopper([&] {
    std::this_thread::sleep_for(150ms);
    source.request_stop();
  });
  const auto start = std::chrono::steady_clock::now();
  auto r = identify(jrpcEndpoint("127.0.0.1", mock.port), options, source.get_token());
  const auto took = std::chrono::steady_clock::now() - start;
  stopper.join();
  CHECK_ERR(r, ErrorCode::Cancelled);
  CHECK_MSG(took < 10s, "the stop did not end the banner wait");
}

TEST(JrpcIdentify, TheNinthConnectionWaitsForASlotAndTimesOut) {
  JrpcMockOptions o;
  o.connectionLimit = 1;
  TcpMock mock(o);
  if (!mock.skipReason.empty()) SKIP(mock.skipReason);
  auto holder = identify(jrpcEndpoint("127.0.0.1", mock.port), quick());
  REQUIRE_OK(holder);
  auto first = JrpcClient::connect(jrpcEndpoint("127.0.0.1", mock.port), quick());
  REQUIRE_OK(first);
  // The only slot is taken: the next connection gets no banner.
  auto r = identify(jrpcEndpoint("127.0.0.1", mock.port), quick());
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK_MSG(r.error().message.find("8 connections") != std::string::npos, r.error().message);
}

// --- JrpcProbeProvider ---

namespace {

// One mock per host name; a host with no mock is refused. Every connect is recorded.
struct Network {
  std::map<std::string, std::unique_ptr<JrpcMockServer>> hosts;
  std::vector<net::Endpoint> seen;

  JrpcMockServer &add(const std::string &host, JrpcMockOptions options = {}) {
    auto &slot = hosts[host];
    slot = std::make_unique<JrpcMockServer>(std::move(options));
    return *slot;
  }

  JrpcProbeProvider::Connector connector() {
    return [this](const net::Endpoint &e) -> Result<net::TransportPtr> {
      seen.push_back(e);
      auto it = hosts.find(e.host);
      if (it == hosts.end()) return fail(ErrorCode::ConnectFailed, "connection refused: " + e.host);
      return it->second->connect();
    };
  }
};

} // namespace

TEST(JrpcProbeProvider, NamesItselfAndReportsOnlyJrpcHosts) {
  Network net;
  net.add("10.0.0.1");
  JrpcMockOptions wrong;
  wrong.banner = "SSH-2.0-OpenSSH";
  net.add("10.0.0.2", wrong);
  JrpcMockOptions silent;
  silent.sendBanner = false;
  net.add("10.0.0.3", silent);
  auto &debug = net.add("10.0.0.4");
  debug.inject(JrpcFault::debugLine().onGreeting());
  net.add("10.0.0.6");
  // 10.0.0.5 is refused.

  std::vector<net::Endpoint> candidates;
  for (const char *host : {"10.0.0.1", "10.0.0.2", "10.0.0.3", "10.0.0.4", "10.0.0.5", "10.0.0.6"}) {
    candidates.push_back(jrpcEndpoint(host, host == std::string("10.0.0.6") ? 1500 : 0, "tcp"));
  }
  JrpcProbeProvider provider(candidates, quick(), net.connector());
  CHECK_EQ(provider.name(), std::string("jrpc"));

  auto found = provider.discover(10s, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{2});
  const auto &first = (*found)[0];
  CHECK_EQ(first.protocol, std::string("jrpc"));
  CHECK_EQ(first.address, std::string("10.0.0.1"));
  CHECK_EQ(first.info.at("port"), std::string("1409"));
  CHECK_EQ(first.info.at("banner"), std::string("JRPC2 connected"));
  CHECK(first.info.at("endpoint").find("10.0.0.1:1409") != std::string::npos);
  CHECK(first.lastSeen.time_since_epoch().count() != 0);
  const auto &second = (*found)[1];
  CHECK_EQ(second.address, std::string("10.0.0.6"));
  CHECK_EQ(second.info.at("port"), std::string("1500"));
  // Every candidate was tried, in order.
  CHECK_EQ(net.seen.size(), size_t{6});
}

// The probe's own timeout is cut to the time left in the run; that must not show up in
// the endpoint reported for the device.
TEST(JrpcProbeProvider, EndpointTextIsTheCandidatesNotTheCutDownProbeTimeout) {
  Network net;
  net.add("10.0.0.1");
  net::Endpoint candidate = jrpcEndpoint("10.0.0.1", 0, "tcp");
  candidate.timeout = net::Endpoint{}.timeout; // the default, which toString() leaves out
  JrpcProbeProvider provider({candidate}, quick(), net.connector());
  auto found = provider.discover(2s, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  const std::string text = (*found)[0].info.at("endpoint");
  CHECK(text.find("10.0.0.1:1409") != std::string::npos);
  CHECK(text.find("timeout=") == std::string::npos);
}

TEST(JrpcProbeProvider, ProbingSendsNoCommandOnlyBye) {
  Network net;
  auto &mock = net.add("10.0.0.1");
  JrpcProbeProvider provider({jrpcEndpoint("10.0.0.1")}, quick(), net.connector());
  auto found = provider.discover(10s, false);
  REQUIRE_OK(found);
  CHECK_EQ(found->size(), size_t{1});
  REQUIRE(mock.waitForCommands(1));
  CHECK_EQ(commandNames(mock), std::vector<std::string>{"bye"});
  CHECK(mock.waitForActiveConnections(0));
}

TEST(JrpcProbeProvider, StopAfterFirstDoesNotProbeTheRest) {
  Network net;
  net.add("10.0.0.1");
  net.add("10.0.0.2");
  JrpcProbeProvider provider({jrpcEndpoint("10.0.0.1"), jrpcEndpoint("10.0.0.2")}, quick(), net.connector());
  auto found = provider.discover(10s, true);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ(net.seen.size(), size_t{1});
  auto all = provider.discover(10s, false);
  REQUIRE_OK(all);
  CHECK_EQ(all->size(), size_t{2});
}

TEST(JrpcProbeProvider, NoJrpcHostsIsAnEmptyListNotAnError) {
  Network net;
  JrpcProbeProvider provider({jrpcEndpoint("10.0.0.1"), jrpcEndpoint("10.0.0.2")}, quick(), net.connector());
  auto found = provider.discover(10s, false);
  REQUIRE_OK(found);
  CHECK(found->empty());
  JrpcProbeProvider none({}, quick(), net.connector());
  auto nothing = none.discover(10s, false);
  REQUIRE_OK(nothing);
  CHECK(nothing->empty());
}

TEST(JrpcProbeProvider, ABadCandidateIsAnErrorOnlyWhenEveryCandidateIsBad) {
  auto bad = [](const net::Endpoint &) -> Result<net::TransportPtr> {
    return fail(ErrorCode::InvalidArgument, "no such host");
  };
  JrpcProbeProvider allBad({jrpcEndpoint("nowhere.invalid")}, quick(), bad);
  CHECK_ERR(allBad.discover(10s, false), ErrorCode::InvalidArgument);

  Network net;
  net.add("10.0.0.1");
  auto mixed = [&](const net::Endpoint &e) -> Result<net::TransportPtr> {
    if (e.host == "nowhere.invalid") return bad(e);
    return net.connector()(e);
  };
  JrpcProbeProvider some({jrpcEndpoint("nowhere.invalid"), jrpcEndpoint("10.0.0.1")}, quick(), mixed);
  auto found = some.discover(10s, false);
  REQUIRE_OK(found);
  CHECK_EQ(found->size(), size_t{1});
}

TEST(JrpcProbeProvider, AnExhaustedRunStopsProbing) {
  Network net;
  net.add("10.0.0.1");
  JrpcProbeProvider provider({jrpcEndpoint("10.0.0.1")}, quick(), net.connector());
  auto found = provider.discover(0ms, false);
  REQUIRE_OK(found);
  CHECK(found->empty());
  CHECK(net.seen.empty());
}

TEST(JrpcProbeProvider, AnEffectivelyInfiniteTimeoutStillProbes) {
  Network net;
  net.add("10.0.0.1");
  JrpcProbeProvider provider({jrpcEndpoint("10.0.0.1")}, quick(), net.connector());
  auto found = provider.discover(std::chrono::milliseconds::max(), false);
  REQUIRE_OK(found);
  CHECK_EQ(found->size(), size_t{1});
}

TEST(JrpcProbeProvider, TheRunTimeoutBoundsASilentCandidate) {
  Network net;
  JrpcMockOptions silent;
  silent.sendBanner = false;
  net.add("10.0.0.1", silent);
  ClientOptions options = quick();
  options.bannerTimeout = 30s;
  JrpcProbeProvider provider({jrpcEndpoint("10.0.0.1")}, options, net.connector());
  const auto start = std::chrono::steady_clock::now();
  auto found = provider.discover(400ms, false);
  const auto took = std::chrono::steady_clock::now() - start;
  REQUIRE_OK(found);
  CHECK(found->empty());
  CHECK_MSG(took < 5s, "the run timeout did not cut the banner wait");
}

TEST(JrpcProbeProvider, ProbesOverRealTcpWithoutAConnector) {
  TcpMock up;
  if (!up.skipReason.empty()) SKIP(up.skipReason);
  JrpcMockOptions wrong;
  wrong.banner = "HTTP/1.1 400 Bad Request";
  TcpMock other(wrong);
  const uint16_t closedPort = ut::unusedLoopbackPort();
  if (closedPort == 0) SKIP("no loopback port");
  JrpcProbeProvider provider({jrpcEndpoint("127.0.0.1", closedPort, "tcp"), jrpcEndpoint("127.0.0.1", other.port, "tcp"),
                              jrpcEndpoint("127.0.0.1", up.port, "tcp")},
                             quick());
  auto found = provider.discover(10s, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ((*found)[0].address, std::string("127.0.0.1"));
  CHECK_EQ((*found)[0].info.at("port"), std::to_string(up.port));
}

TEST(JrpcProbeProvider, IsNotRegisteredByRegisterBuiltins) {
  registerBuiltins();
  for (const auto &p : discovery::DiscoveryRegistry::instance().providers()) CHECK(p->name() != "jrpc");
}
