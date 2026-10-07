#include "protocols/xbdm/integration_support.hpp"
#include "support/loopback_server.hpp"

#include <discovery/discovery.hpp>
#include <net/transport_registry.hpp>
#include <protocols/xbdm/discovery.hpp>

#include <stop_token>
#include <thread>

// XbdmDiscovery against the mock's name responder (section 2): in memory through
// the mock's datagram sockets, and over real UDP on loopback.

using namespace xit;
using updclient::xbdm::DiscoveryOptions;
using updclient::xbdm::XbdmDiscovery;

namespace {

DiscoveryOptions quickDiscovery() {
  DiscoveryOptions o;
  o.nameQuery = quickOptions();
  return o;
}

XbdmDiscovery inMemory(XbdmMockServer &mock, DiscoveryOptions options = quickDiscovery()) {
  return XbdmDiscovery(mock.datagramFactory("127.0.0.1"), options,
                       [&mock](const updclient::net::Endpoint &) { return mock.connect(); });
}

} // namespace

TEST(XbdmDiscoveryIntegration, WildcardFindsTheMockAndAsksItsName) {
  XbdmMockServer mock;
  auto discovery = inMemory(mock);
  auto devices = discovery.discover(300ms, false);
  REQUIRE_OK(devices);
  REQUIRE_EQ(devices->size(), size_t{1});
  const auto &d = devices->front();
  CHECK_EQ(d.protocol, std::string("xbdm"));
  CHECK_EQ(d.address, std::string("127.0.0.1"));
  CHECK_EQ(d.info.at("udpName"), std::string("MockDevkit"));
  CHECK_EQ(d.info.at("name"), std::string("MockDevkit"));
  CHECK_EQ(d.info.at("port"), std::string("730"));
  CHECK_EQ(mock.nameRequestsSeen(), size_t{3});
  CHECK_EQ(mock.commandLines(), (std::vector<std::string>{"dbgname", "bye"}));

  auto first = discovery.discover(300ms, true);
  REQUIRE_OK(first);
  CHECK_EQ(first->size(), size_t{1});
}

TEST(XbdmDiscoveryIntegration, TheTcpNameWinsAndAFailedQueryKeepsTheUdpName) {
  XbdmMockServer mock;
  auto info = mock.info();
  info.debugName = "UdpName";
  mock.setInfo(info);
  mock.inject(XbdmFault::statusLine("200- TcpName").on("dbgname"));
  auto devices = inMemory(mock).discover(200ms, true);
  REQUIRE_OK(devices);
  REQUIRE_EQ(devices->size(), size_t{1});
  CHECK_EQ(devices->front().info.at("name"), std::string("TcpName"));
  CHECK_EQ(devices->front().info.at("udpName"), std::string("UdpName"));

  XbdmDiscovery unreachable(mock.datagramFactory("127.0.0.1"), quickDiscovery(),
                            [](const updclient::net::Endpoint &) -> Result<updclient::net::TransportPtr> {
                              return updclient::fail(ErrorCode::ConnectFailed, "refused");
                            });
  auto udpOnly = unreachable.discover(200ms, true);
  REQUIRE_OK(udpOnly);
  REQUIRE_EQ(udpOnly->size(), size_t{1});
  CHECK_EQ(udpOnly->front().info.at("name"), std::string("UdpName"));

  auto noQuery = quickDiscovery();
  noQuery.queryNames = false;
  mock.clearCommands();
  auto quiet = inMemory(mock, noQuery).discover(200ms, true);
  REQUIRE_OK(quiet);
  CHECK(mock.commands().empty());
}

TEST(XbdmDiscoveryIntegration, LookupByName) {
  XbdmMockServer mock;
  auto discovery = inMemory(mock);
  auto found = discovery.findByName("mockDEVKIT", 300ms);
  REQUIRE_OK(found);
  REQUIRE(found->has_value());
  CHECK_EQ((*found)->address, std::string("127.0.0.1"));
  auto other = discovery.findByName("SomeoneElse", 200ms);
  REQUIRE_OK(other);
  CHECK(!other->has_value());
  CHECK_ERR(discovery.findByName("", 100ms), ErrorCode::InvalidArgument);
}

TEST(XbdmDiscoveryIntegration, ProbeOneAddress) {
  XbdmMockServer mock;
  auto discovery = inMemory(mock);
  auto there = discovery.probeAddress("127.0.0.1", 300ms);
  REQUIRE_OK(there);
  CHECK(there->has_value());
  auto elsewhere = discovery.probeAddress("10.9.8.7", 200ms);
  REQUIRE_OK(elsewhere);
  CHECK(!elsewhere->has_value());
}

TEST(XbdmDiscoveryIntegration, SilentAndMalformedReplies) {
  XbdmMockServer mock;
  for (const auto mode : {ut::XbdmUdpMode::Silent, ut::XbdmUdpMode::WrongType, ut::XbdmUdpMode::LengthBeyondDatagram}) {
    mock.setUdpMode(mode);
    auto devices = inMemory(mock).discover(200ms, false);
    REQUIRE_OK(devices);
    CHECK(devices->empty());
  }
  mock.setUdpMode(ut::XbdmUdpMode::TrailingBytes);
  auto trailing = inMemory(mock).discover(200ms, true);
  REQUIRE_OK(trailing);
  REQUIRE_EQ(trailing->size(), size_t{1});
  CHECK_EQ(trailing->front().info.at("udpName"), std::string("MockDevkit"));
}

TEST(XbdmDiscoveryIntegration, OverRealUdpOnLoopback) {
  XbdmMockServer mock;
  auto tcpPort = mock.listenTcp();
  if (!tcpPort) SKIP("loopback TCP refused: " + updclient::formatError(tcpPort.error()));
  auto udpPort = mock.listenUdp();
  if (!udpPort) SKIP("loopback UDP refused: " + updclient::formatError(udpPort.error()));
  DiscoveryOptions options = quickDiscovery();
  options.broadcastAddress = "127.0.0.1";
  options.port = *udpPort;
  const uint16_t port = *tcpPort;
  XbdmDiscovery discovery({}, options, [port](const updclient::net::Endpoint &e) {
    return updclient::net::TcpTransport::connect(e.host, port, 2000ms);
  });
  auto devices = discovery.discover(600ms, true);
  REQUIRE_OK(devices);
  REQUIRE_EQ(devices->size(), size_t{1});
  CHECK_EQ(devices->front().address, std::string("127.0.0.1"));
  CHECK_EQ(devices->front().info.at("name"), std::string("MockDevkit"));
  auto byName = discovery.findByName("MOCKDEVKIT", 600ms);
  REQUIRE_OK(byName);
  CHECK(byName->has_value());
  CHECK(mock.nameRequestsSeen() >= 2);
}

TEST(XbdmDiscoveryIntegration, AStopEndsARealLoopbackSearch) {
  XbdmMockServer mock;
  auto udpPort = mock.listenUdp();
  if (!udpPort) SKIP("loopback UDP refused: " + updclient::formatError(udpPort.error()));
  DiscoveryOptions options = quickDiscovery();
  options.broadcastAddress = "127.0.0.1";
  options.port = *udpPort;
  options.queryNames = false;
  XbdmDiscovery discovery({}, options);
  std::stop_source source;
  std::thread stopper([&] {
    std::this_thread::sleep_for(200ms);
    source.request_stop();
  });
  const auto start = std::chrono::steady_clock::now();
  auto devices = discovery.discover(10s, false, source.get_token());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  stopper.join();
  REQUIRE_OK(devices);
  REQUIRE_EQ(devices->size(), size_t{1});
  CHECK_EQ(devices->front().info.at("port"), std::to_string(*udpPort));
  CHECK(elapsed < 2s);
}

TEST(XbdmDiscoveryIntegration, IdentifyByAddress) {
  XbdmMockServer mock;
  auto port = mock.listenTcp();
  if (!port) SKIP("loopback TCP refused: " + updclient::formatError(port.error()));
  auto endpoint = updclient::net::Endpoint::parse("xbdm://127.0.0.1:" + std::to_string(*port));
  REQUIRE_OK(endpoint);
  auto device = updclient::xbdm::identify(*endpoint, quickOptions());
  REQUIRE_OK(device);
  CHECK_EQ(device->protocol, std::string("xbdm"));
  CHECK_EQ(device->info.at("name"), std::string("MockDevkit"));
  CHECK_EQ(device->info.at("port"), std::to_string(*port));

  endpoint->port = ut::unusedLoopbackPort();
  CHECK(!updclient::xbdm::identify(*endpoint, quickOptions()));
}

TEST(XbdmDiscoveryIntegration, RegisteredSchemeReachesTheMock) {
  XbdmMockServer mock;
  auto port = mock.listenTcp();
  if (!port) SKIP("loopback TCP refused: " + updclient::formatError(port.error()));
  updclient::net::TransportRegistry transports;
  updclient::discovery::DiscoveryRegistry providers;
  updclient::xbdm::registerXbdm(transports, providers);
  REQUIRE_EQ(providers.providers().size(), size_t{1});
  CHECK_EQ(providers.providers()[0]->name(), std::string("xbdm"));
  auto endpoint = updclient::net::Endpoint::parse("xbdm://127.0.0.1:" + std::to_string(*port));
  REQUIRE_OK(endpoint);
  auto transport = transports.connect(*endpoint);
  REQUIRE_OK(transport);
  auto client = XbdmClient::attach(std::move(*transport), quickOptions());
  REQUIRE_OK(client);
  CHECK_EQ(client->debugName().value_or(""), std::string("MockDevkit"));
}
