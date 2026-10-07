#include "support/mock_transport.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/xell/discovery.hpp>

#include <map>
#include <memory>

using namespace updclient;
using namespace updclient::xell;
using namespace std::chrono_literals;
using ut::Bytes;
using ut::MockScript;

namespace {

const std::string kCpuHex = "A1B2C3D4E5F60718293A4B5C6D7E8F90";

Bytes page(const std::string &extraHeaders, const std::string &body, const std::string &status = "200 OK") {
  Bytes out = ut::bytesOf("HTTP/1.0 " + status + "\r\n" + extraHeaders + "Content-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body);
  return out;
}

XellProbeResult probe(const Bytes &reply, std::shared_ptr<MockScript> *keep = nullptr) {
  auto script = MockScript::create();
  script->reply(reply);
  auto t = script->transport();
  if (keep) *keep = script;
  return XellDiscovery::probeTransport(*t, "xell");
}

} // namespace

TEST(XellProbe, ServerHeaderNamingXellIsEnough) {
  const auto r = probe(page("Server: XeLL-HTTPD/1.0\r\n", ""));
  CHECK(r.isXell);
  CHECK(r.httpResponse);
  CHECK_EQ(r.statusCode, 200);
  CHECK(r.note.find("Server header") != std::string::npos);
}

TEST(XellProbe, ServerHeaderIsMatchedEvenOnAnErrorStatus) {
  const auto r = probe(page("Server: xell\r\n", "", "404 Not Found"));
  CHECK(r.isXell);
  CHECK_EQ(r.statusCode, 404);
}

TEST(XellProbe, IndexPageMentioningXellIsEnough) {
  const auto r = probe(page("", "<html><title>XeLL</title></html>"));
  CHECK(r.isXell);
  CHECK(r.note.find("mentions XeLL") != std::string::npos);
}

TEST(XellProbe, IndexPageListingACpuKeyIsEnough) {
  const auto r = probe(page("", "<p>CPU Key: " + kCpuHex + "</p>"));
  CHECK(r.isXell);
  CHECK(r.note.find("CPU key") != std::string::npos);
}

TEST(XellProbe, PlainOkIsNotEnough) {
  const auto r = probe(page("Server: nginx\r\n", "<html>Welcome to my router</html>"));
  CHECK(!r.isXell);
  CHECK(r.httpResponse);
  CHECK_EQ(r.statusCode, 200);
  CHECK(r.note.find("without XeLL marker") != std::string::npos);
}

TEST(XellProbe, ACpuWordWithoutAKeyIsNotEnough) {
  const auto r = probe(page("", "<p>CPU usage: 3%</p>"));
  CHECK(!r.isXell);
}

TEST(XellProbe, ASixtyFourHexRunNextToTheCpuLabelIsNotAKey) {
  const auto r = probe(page("", "<p>CPU: " + kCpuHex + kCpuHex + "</p>"));
  CHECK(!r.isXell);
}

TEST(XellProbe, NotFoundWithoutMarker) {
  const auto r = probe(page("", "nothing here", "404 Not Found"));
  CHECK(!r.isXell);
  CHECK(r.httpResponse);
  CHECK_EQ(r.statusCode, 404);
}

TEST(XellProbe, NonHttpReplyIsNegativeWithoutAnHttpResponse) {
  const auto r = probe(ut::bytesOf("SSH-2.0-OpenSSH_9.0\r\n"));
  CHECK(!r.isXell);
  CHECK(!r.httpResponse);
  CHECK(r.note.find("no HTTP response") != std::string::npos);
}

TEST(XellProbe, SilentPeerIsNegative) {
  auto script = MockScript::create();
  auto t = script->transport();
  const auto r = XellDiscovery::probeTransport(*t, "xell");
  CHECK(!r.isXell);
  CHECK(!r.httpResponse);
}

TEST(XellProbe, TimeoutIsNegative) {
  auto script = MockScript::create();
  script->replyTimeout();
  auto t = script->transport();
  const auto r = XellDiscovery::probeTransport(*t, "xell");
  CHECK(!r.isXell);
  CHECK(r.note.find("Timeout") != std::string::npos);
}

TEST(XellProbe, SendFailureIsNegative) {
  auto script = MockScript::create();
  script->failWrites(makeError(ErrorCode::Io, "broken pipe"));
  auto t = script->transport();
  const auto r = XellDiscovery::probeTransport(*t, "xell");
  CHECK(!r.isXell);
  CHECK(r.note.find("request failed") != std::string::npos);
}

TEST(XellProbe, BodyThatTimesOutAfterAnUnmarkedHeadIsNegativeButKeepsTheStatus) {
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\nContent-Length: 100\r\n\r\nabc").replyTimeout();
  auto t = script->transport();
  const auto r = XellDiscovery::probeTransport(*t, "xell");
  CHECK(!r.isXell);
  CHECK(r.httpResponse);
  CHECK_EQ(r.statusCode, 200);
}

TEST(XellProbe, SendsAProbeUserAgentAndTheGivenHost) {
  std::shared_ptr<MockScript> script;
  probe(page("", ""), &script);
  const auto text = script->writtenText();
  CHECK(text.rfind("GET / HTTP/1.0\r\n", 0) == 0);
  CHECK(text.find("Host: xell\r\n") != std::string::npos);
  CHECK(text.find("User-Agent: UpdClient-Probe\r\n") != std::string::npos);
}

TEST(XellProbe, DoesNotReadMoreThanAProbeNeeds) {
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\n\r\nplain page");
  script->reply(ut::patternBytes(200000));
  script->replyTimeout();
  auto t = script->transport();
  const auto r = XellDiscovery::probeTransport(*t, "xell");
  CHECK(!r.isXell);
  CHECK(script->unreadBytes() > 0);
}

TEST(XellProbeDetailed, ConnectsThroughTheGivenConnectorOnPort80ByDefault) {
  net::Endpoint seen;
  auto script = MockScript::create();
  script->reply(page("Server: XeLL\r\n", ""));
  net::Endpoint endpoint;
  endpoint.scheme = "tcp";
  endpoint.host = "10.0.0.2";
  auto r = XellDiscovery::probeDetailed(endpoint, [&](const net::Endpoint &e) -> Result<net::TransportPtr> {
    seen = e;
    return net::TransportPtr(script->transport());
  });
  REQUIRE_OK(r);
  CHECK(r->isXell);
  CHECK_EQ(seen.port, 80);
  CHECK(script->writtenText().find("Host: 10.0.0.2\r\n") != std::string::npos);
}

TEST(XellProbeDetailed, NonDefaultPortGoesIntoTheHostHeader) {
  auto script = MockScript::create();
  script->reply(page("Server: XeLL\r\n", ""));
  net::Endpoint endpoint;
  endpoint.scheme = "tcp";
  endpoint.host = "10.0.0.2";
  endpoint.port = 8080;
  auto r = XellDiscovery::probeDetailed(endpoint, [&](const net::Endpoint &) -> Result<net::TransportPtr> {
    return net::TransportPtr(script->transport());
  });
  REQUIRE_OK(r);
  CHECK(script->writtenText().find("Host: 10.0.0.2:8080\r\n") != std::string::npos);
}

TEST(XellProbeDetailed, UnreachableHostsAreNegativeNotErrors) {
  for (ErrorCode code : {ErrorCode::ConnectFailed, ErrorCode::Timeout, ErrorCode::Disconnected, ErrorCode::Io}) {
    net::Endpoint endpoint;
    endpoint.scheme = "tcp";
    endpoint.host = "10.0.0.2";
    auto r = XellDiscovery::probeDetailed(endpoint, [&](const net::Endpoint &) -> Result<net::TransportPtr> {
      return fail(code, "nope");
    });
    REQUIRE_OK(r);
    CHECK(!r->isXell);
    CHECK(r->note.find("unreachable") != std::string::npos);
  }
}

TEST(XellProbeDetailed, ProblemsWithTheRequestItselfAreErrors) {
  net::Endpoint endpoint;
  endpoint.scheme = "carrier-pigeon";
  endpoint.host = "roof";
  auto r = XellDiscovery::probeDetailed(endpoint, [](const net::Endpoint &) -> Result<net::TransportPtr> {
    return fail(ErrorCode::Unsupported, "no such transport");
  });
  CHECK_ERR(r, ErrorCode::Unsupported);

  auto viaRegistry = XellDiscovery::probeDetailed(endpoint);
  CHECK_ERR(viaRegistry, ErrorCode::Unsupported);
}

TEST(XellProbeDetailed, NullTransportIsNegative) {
  net::Endpoint endpoint;
  endpoint.scheme = "tcp";
  endpoint.host = "h";
  auto r = XellDiscovery::probeDetailed(endpoint, [](const net::Endpoint &) -> Result<net::TransportPtr> {
    return net::TransportPtr();
  });
  REQUIRE_OK(r);
  CHECK(!r->isXell);
}

TEST(XellProbeDetailed, BoolWrapperFollowsTheVerdict) {
  net::Endpoint endpoint;
  endpoint.scheme = "tcp";
  endpoint.host = "h";
  auto yes = XellDiscovery::probe(endpoint, [](const net::Endpoint &) -> Result<net::TransportPtr> {
    auto script = MockScript::create();
    script->reply(page("Server: XeLL\r\n", ""));
    return net::TransportPtr(script->transport());
  });
  REQUIRE_OK(yes);
  CHECK(*yes);
  auto no = XellDiscovery::probe(endpoint, [](const net::Endpoint &) -> Result<net::TransportPtr> {
    auto script = MockScript::create();
    script->reply(page("", "hello"));
    return net::TransportPtr(script->transport());
  });
  REQUIRE_OK(no);
  CHECK(!*no);
}

namespace {

XellDiscovery::Connector connectorByHost(std::map<std::string, std::string> pagesByHost,
                                         std::vector<net::Endpoint> *seen = nullptr) {
  return [pagesByHost = std::move(pagesByHost), seen](const net::Endpoint &e) -> Result<net::TransportPtr> {
    if (seen) seen->push_back(e);
    auto it = pagesByHost.find(e.host);
    if (it == pagesByHost.end()) return fail(ErrorCode::ConnectFailed, "connection refused");
    auto script = MockScript::create();
    script->reply(page(it->second, ""));
    return net::TransportPtr(script->transport());
  };
}

net::Endpoint tcpEndpoint(const std::string &host, uint16_t port = 0) {
  net::Endpoint e;
  e.scheme = "tcp";
  e.host = host;
  e.port = port;
  return e;
}

} // namespace

TEST(XellProbeProvider, NamesItselfAndReportsOnlyXellHosts) {
  XellProbeProvider provider({tcpEndpoint("10.0.0.1"), tcpEndpoint("10.0.0.2", 8080), tcpEndpoint("10.0.0.3")},
                             connectorByHost({{"10.0.0.1", "Server: nginx\r\n"}, {"10.0.0.2", "Server: XeLL\r\n"}}));
  CHECK_EQ(provider.name(), std::string("xell"));

  auto found = provider.discover(5s, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  const auto &device = (*found)[0];
  CHECK_EQ(device.protocol, std::string("xell"));
  CHECK_EQ(device.address, std::string("10.0.0.2"));
  CHECK_EQ(device.info.at("port"), std::string("8080"));
  CHECK(device.info.at("endpoint").find("tcp://10.0.0.2:8080") == 0);
  CHECK(!device.info.at("evidence").empty());
  CHECK(device.lastSeen.time_since_epoch().count() != 0);
}

TEST(XellProbeProvider, DefaultPortIsReportedAs80) {
  XellProbeProvider provider({tcpEndpoint("10.0.0.1")}, connectorByHost({{"10.0.0.1", "Server: XeLL\r\n"}}));
  auto found = provider.discover(5s, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ((*found)[0].info.at("port"), std::string("80"));
}

TEST(XellProbeProvider, EffectivelyInfiniteTimeoutStillProbes) {
  XellProbeProvider provider({tcpEndpoint("10.0.0.1")}, connectorByHost({{"10.0.0.1", "Server: XeLL\r\n"}}));
  auto found = provider.discover(std::chrono::milliseconds::max(), false);
  REQUIRE_OK(found);
  CHECK_EQ(found->size(), size_t{1});
}

TEST(XellProbeProvider, PortlessSchemeCandidateReportsNoPort) {
  net::Endpoint serial;
  serial.scheme = "serialprobe";
  serial.host = "/dev/ttyUSB0";
  std::vector<net::Endpoint> seen;
  XellProbeProvider provider({serial}, connectorByHost({{"/dev/ttyUSB0", "Server: XeLL\r\n"}}, &seen));
  auto found = provider.discover(5s, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK(found.value()[0].info.find("port") == found.value()[0].info.end());
  REQUIRE_EQ(seen.size(), size_t{1});
  CHECK_EQ(seen[0].port, 0);
}

TEST(XellProbeProvider, ReportsEveryXellHostByDefault) {
  XellProbeProvider provider({tcpEndpoint("10.0.0.1"), tcpEndpoint("10.0.0.2")},
                             connectorByHost({{"10.0.0.1", "Server: XeLL\r\n"}, {"10.0.0.2", "Server: XeLL\r\n"}}));
  auto found = provider.discover(5s, false);
  REQUIRE_OK(found);
  CHECK_EQ(found->size(), size_t{2});
}

TEST(XellProbeProvider, StopAfterFirstDoesNotProbeTheRest) {
  std::vector<net::Endpoint> seen;
  XellProbeProvider provider({tcpEndpoint("10.0.0.1"), tcpEndpoint("10.0.0.2")},
                             connectorByHost({{"10.0.0.1", "Server: XeLL\r\n"}, {"10.0.0.2", "Server: XeLL\r\n"}}, &seen));
  auto found = provider.discover(5s, true);
  REQUIRE_OK(found);
  CHECK_EQ(found->size(), size_t{1});
  CHECK_EQ(seen.size(), size_t{1});
}

TEST(XellProbeProvider, UnreachableCandidatesAreNotErrors) {
  XellProbeProvider provider({tcpEndpoint("10.0.0.1"), tcpEndpoint("10.0.0.2")}, connectorByHost({}));
  auto found = provider.discover(5s, false);
  REQUIRE_OK(found);
  CHECK(found->empty());
}

TEST(XellProbeProvider, EveryCandidateFailingWithARealErrorIsAnError) {
  XellProbeProvider provider({tcpEndpoint("a"), tcpEndpoint("b")},
                             [](const net::Endpoint &) -> Result<net::TransportPtr> {
                               return fail(ErrorCode::Unsupported, "no such transport");
                             });
  CHECK_ERR(provider.discover(5s, false), ErrorCode::Unsupported);
}

TEST(XellProbeProvider, OneBadCandidateDoesNotHideAGoodOne) {
  auto good = connectorByHost({{"good", "Server: XeLL\r\n"}});
  XellProbeProvider provider({tcpEndpoint("bad"), tcpEndpoint("good")},
                             [good](const net::Endpoint &e) -> Result<net::TransportPtr> {
                               if (e.host == "bad") return fail(ErrorCode::Unsupported, "bad scheme");
                               return good(e);
                             });
  auto found = provider.discover(5s, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ((*found)[0].address, std::string("good"));
}

TEST(XellProbeProvider, NoCandidatesGivesAnEmptyResult) {
  XellProbeProvider provider({}, connectorByHost({}));
  auto found = provider.discover(5s, false);
  REQUIRE_OK(found);
  CHECK(found->empty());
}

TEST(XellProbeProvider, EachProbeIsBoundedByTheRemainingBudget) {
  std::vector<net::Endpoint> seen;
  XellProbeProvider provider({tcpEndpoint("10.0.0.1")}, connectorByHost({}, &seen));
  REQUIRE_OK(provider.discover(200ms, false));
  REQUIRE_EQ(seen.size(), size_t{1});
  CHECK(seen[0].timeout <= 200ms);
  CHECK(seen[0].timeout > 0ms);
}

TEST(XellProbeProvider, ZeroBudgetProbesNothing) {
  std::vector<net::Endpoint> seen;
  XellProbeProvider provider({tcpEndpoint("10.0.0.1")}, connectorByHost({{"10.0.0.1", "Server: XeLL\r\n"}}, &seen));
  auto found = provider.discover(0ms, false);
  REQUIRE_OK(found);
  CHECK(found->empty());
  CHECK(seen.empty());
}
