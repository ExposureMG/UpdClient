#include "support/mock_transport.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <updclient/core/hex.hpp>
#include <updclient/net/transport_registry.hpp>
#include <updclient/protocols/xell/client.hpp>

#include <array>
#include <memory>
#include <vector>

using namespace updclient;
using namespace updclient::xell;
using ut::Bytes;
using ut::MockScript;

namespace {

const std::string kCpuHex = "A1B2C3D4E5F60718293A4B5C6D7E8F90";
const std::string kDvdHex = "00112233445566778899AABBCCDDEEFF";

std::array<uint8_t, 16> keyBytes(const std::string &hex) {
  std::array<uint8_t, 16> out{};
  REQUIRE_OK(parseHex(hex, out));
  return out;
}

std::string infoPage() {
  return "<html><body bgcolor=\"#102030\" text=\"#FFEEDD\"><h1>XeLL</h1>"
         "<p>CPU Key: " + kCpuHex + "</p><p>DVD Key: " + kDvdHex + "</p></body></html>";
}

Bytes httpResponse(const std::string &status, const Bytes &body, bool withLength = true) {
  std::string head = "HTTP/1.0 " + status + "\r\n";
  if (withLength) head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  head += "Connection: close\r\n\r\n";
  Bytes out = ut::bytesOf(head);
  ut::append(out, body);
  return out;
}

Bytes okPage(const std::string &html) {
  return httpResponse("200 OK", ut::bytesOf(html));
}

// Hands out one scripted transport per request and remembers how it was used.
struct Scripted {
  std::vector<std::shared_ptr<MockScript>> scripts;
  size_t calls = 0;

  std::shared_ptr<MockScript> add() {
    scripts.push_back(MockScript::create());
    return scripts.back();
  }
};

XellClient::Connector connectorFor(const std::shared_ptr<Scripted> &state) {
  return [state]() -> Result<net::TransportPtr> {
    if (state->calls >= state->scripts.size()) return fail(ErrorCode::ConnectFailed, "no more scripted connections");
    return net::TransportPtr(state->scripts[state->calls++]->transport());
  };
}

} // namespace

TEST(XellInfo, ParsesKeysAndColours) {
  const auto info = parseXellInfo(infoPage());
  CHECK(info.cpuKeyValid);
  CHECK(info.dvdKeyValid);
  CHECK(info.cpuKey == keyBytes(kCpuHex));
  CHECK(info.dvdKey == keyBytes(kDvdHex));
  CHECK_EQ(info.bgColor, std::string("#102030"));
  CHECK_EQ(info.fgColor, std::string("#FFEEDD"));
  CHECK(info.missingFields().empty());
}

TEST(XellInfo, LabelsPickTheSlotRegardlessOfOrder) {
  const auto info = parseXellInfo("DVD Key: " + kDvdHex + "<br>CPU Key: " + kCpuHex);
  CHECK(info.cpuKey == keyBytes(kCpuHex));
  CHECK(info.dvdKey == keyBytes(kDvdHex));
}

TEST(XellInfo, UnlabelledKeysAreCpuThenDvd) {
  const auto info = parseXellInfo(kCpuHex + "\n" + kDvdHex + "\n");
  CHECK(info.cpuKeyValid);
  CHECK(info.dvdKeyValid);
  CHECK(info.cpuKey == keyBytes(kCpuHex));
  CHECK(info.dvdKey == keyBytes(kDvdHex));
}

TEST(XellInfo, KeyMustNotMatchInsideA64HexRun) {
  const std::string run64 = kCpuHex + kDvdHex;
  const auto info = parseXellInfo("<p>Fuses: " + run64 + "</p>");
  CHECK(!info.cpuKeyValid);
  CHECK(!info.dvdKeyValid);
}

TEST(XellInfo, SixtyFourHexRunDoesNotDisturbRealKeys) {
  const std::string run64 = kDvdHex + kDvdHex;
  const auto info = parseXellInfo("CPU Key: " + kCpuHex + "<p>Fuse line: " + run64 + "</p>");
  CHECK(info.cpuKeyValid);
  CHECK(info.cpuKey == keyBytes(kCpuHex));
  CHECK(!info.dvdKeyValid);
}

TEST(XellInfo, RunsOfOtherLengthsAreNotKeys) {
  CHECK(!parseXellInfo(kCpuHex + "0").cpuKeyValid);
  CHECK(!parseXellInfo(kCpuHex.substr(1)).cpuKeyValid);
  CHECK(!parseXellInfo("0" + kCpuHex).cpuKeyValid);
}

TEST(XellInfo, KeyGluedToAWordCharacterIsRejected) {
  CHECK(!parseXellInfo("x" + kCpuHex).cpuKeyValid);
  CHECK(!parseXellInfo(kCpuHex + "_").cpuKeyValid);
  CHECK(parseXellInfo(":" + kCpuHex + ";").cpuKeyValid);
}

TEST(XellInfo, KeyAtTheVeryStartAndEndOfTheText) {
  CHECK(parseXellInfo(kCpuHex).cpuKeyValid);
}

TEST(XellInfo, LowercaseHexIsAccepted) {
  std::string lower = kCpuHex;
  for (auto &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const auto info = parseXellInfo("cpu key " + lower);
  CHECK(info.cpuKeyValid);
  CHECK(info.cpuKey == keyBytes(kCpuHex));
}

TEST(XellInfo, HtmlCharacterReferencesAreNotColours) {
  const auto info = parseXellInfo("&#123456; &#abcdef; <font color=#00ff00>");
  CHECK_EQ(info.bgColor, std::string("#00ff00"));
  CHECK(info.fgColor.empty());
}

TEST(XellInfo, ColourNeedsExactlySixHexDigits) {
  CHECK(parseXellInfo("#12345").bgColor.empty());
  CHECK(parseXellInfo("#1234567").bgColor.empty());
  CHECK_EQ(parseXellInfo(" #123456 ").bgColor, std::string("#123456"));
}

TEST(XellInfo, MissingFieldsAreNamed) {
  const auto info = parseXellInfo("nothing useful here");
  const auto missing = info.missingFields();
  CHECK_EQ(missing, (std::vector<std::string>{"cpuKey", "dvdKey", "bgColor", "fgColor"}));
}

TEST(XellInfo, EmptyInput) {
  const auto info = parseXellInfo("");
  CHECK(!info.cpuKeyValid);
  CHECK(info.bgColor.empty());
}

TEST(XellClient, GetInfoParsesTheIndexPage) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply(okPage(infoPage()));
  XellClient client(connectorFor(state));

  auto info = client.getInfo();
  REQUIRE_OK(info);
  CHECK(info->cpuKey == keyBytes(kCpuHex));
  CHECK(info->dvdKey == keyBytes(kDvdHex));
  CHECK_EQ(info->bgColor, std::string("#102030"));
  REQUIRE_EQ(state->calls, size_t{1});
  CHECK_EQ(state->scripts[0]->writtenText().rfind("GET / HTTP/1.0\r\nHost: xell\r\n", 0), size_t{0});
}

TEST(XellClient, ClientIsReusableEachRequestOpensAFreshTransport) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply(okPage(infoPage()));
  state->add()->reply(okPage("<p>fuse data</p>"));
  XellClient client(connectorFor(state));

  REQUIRE_OK(client.getInfo());
  auto fuses = client.getFuses();
  REQUIRE_OK(fuses);
  CHECK_EQ(*fuses, std::string("<p>fuse data</p>"));
  CHECK_EQ(state->calls, size_t{2});
  CHECK(state->scripts[0]->writtenText().find("GET / HTTP/1.0") == 0);
  CHECK(state->scripts[1]->writtenText().find("GET /FUSE HTTP/1.0") == 0);
}

TEST(XellClient, SameRequestTwiceInvokesTheConnectorTwice) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply(okPage(infoPage()));
  state->add()->reply(okPage(infoPage()));
  XellClient client(connectorFor(state));
  REQUIRE_OK(client.getInfo());
  REQUIRE_OK(client.getInfo());
  CHECK_EQ(state->calls, size_t{2});
}

TEST(XellClient, ConnectorCallCountIsExactlyOnePerRequest) {
  size_t calls = 0;
  XellClient client([&]() -> Result<net::TransportPtr> {
    ++calls;
    auto script = MockScript::create();
    script->reply(okPage(infoPage()));
    return net::TransportPtr(script->transport());
  });
  REQUIRE_OK(client.getInfo());
  CHECK_EQ(calls, size_t{1});
  REQUIRE_OK(client.getInfo());
  CHECK_EQ(calls, size_t{2});
  REQUIRE_OK(client.getFuses());
  CHECK_EQ(calls, size_t{3});
}

TEST(XellClient, ClientRecoversAfterAFailedRequest) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply("HTTP/1.0 500 Oops\r\n\r\n");
  state->add()->reply(okPage(infoPage()));
  XellClient client(connectorFor(state));
  CHECK_ERR(client.getInfo(), ErrorCode::Protocol);
  CHECK_OK(client.getInfo());
}

TEST(XellClient, HostHeaderIsConfigurable) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply(okPage(infoPage()));
  XellClient client(connectorFor(state), "10.0.0.2:8080");
  REQUIRE_OK(client.getInfo());
  CHECK(state->scripts[0]->writtenText().find("Host: 10.0.0.2:8080\r\n") != std::string::npos);
}

TEST(XellClient, GetInfoWithOnlyColoursIsStillAResult) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply(okPage("<body bgcolor=#001122 text=#334455>no keys yet</body>"));
  XellClient client(connectorFor(state));
  auto info = client.getInfo();
  REQUIRE_OK(info);
  CHECK(!info->cpuKeyValid);
  CHECK_EQ(info->missingFields(), (std::vector<std::string>{"cpuKey", "dvdKey"}));
}

TEST(XellClient, GetInfoOnAnUnrelatedPageIsAProtocolError) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply(okPage("<html>router login</html>"));
  XellClient client(connectorFor(state));
  CHECK_ERR(client.getInfo(), ErrorCode::Protocol);
}

TEST(XellClient, SixtyFourHexRunAloneIsNotMistakenForKeys) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply(okPage("<p>" + kCpuHex + kDvdHex + "</p>"));
  XellClient client(connectorFor(state));
  CHECK_ERR(client.getInfo(), ErrorCode::Protocol);
}

TEST(XellClient, NonOkStatusIsAProtocolError) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply("HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n");
  XellClient client(connectorFor(state));
  const auto r = client.getFuses();
  REQUIRE_ERR(r, ErrorCode::Protocol);
  CHECK(r.error().message.find("404") != std::string::npos);
}

TEST(XellClient, ChunkedResponsesAreReportedAsUnsupported) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
  XellClient client(connectorFor(state));
  CHECK_ERR(client.getInfo(), ErrorCode::Unsupported);
}

TEST(XellClient, OversizedInfoPageIsRejected) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply("HTTP/1.0 200 OK\r\nContent-Length: 2000000\r\n\r\n");
  XellClient client(connectorFor(state));
  CHECK_ERR(client.getInfo(), ErrorCode::LimitExceeded);
}

TEST(XellClient, TimeoutWhileReadingThePageIsReported) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply("HTTP/1.0 200 OK\r\nContent-Length: 500\r\n\r\n<html>").replyTimeout();
  XellClient client(connectorFor(state));
  CHECK_ERR(client.getInfo(), ErrorCode::Timeout);
}

TEST(XellClient, ConnectorErrorsPropagate) {
  XellClient client([]() -> Result<net::TransportPtr> { return fail(ErrorCode::ConnectFailed, "refused", 111); });
  const auto r = client.getInfo();
  REQUIRE_ERR(r, ErrorCode::ConnectFailed);
  CHECK_EQ(r.error().sysError, 111);
  CHECK_ERR(client.getFuses(), ErrorCode::ConnectFailed);
}

TEST(XellClient, NullTransportFromConnectorIsAConnectFailure) {
  XellClient client([]() -> Result<net::TransportPtr> { return net::TransportPtr(); });
  CHECK_ERR(client.getInfo(), ErrorCode::ConnectFailed);
}

TEST(XellClient, MissingConnectorIsNotConnected) {
  XellClient client(XellClient::Connector{});
  CHECK_ERR(client.getInfo(), ErrorCode::NotConnected);
  CHECK_ERR(client.getKeyvault(), ErrorCode::NotConnected);
}

TEST(XellClient, KeyvaultModesUseTheirOwnPaths) {
  struct Case {
    KeyvaultMode mode;
    const char *path;
  };
  for (const Case &c : {Case{KeyvaultMode::Decrypted, "/KV"}, Case{KeyvaultMode::Raw, "/KVRAW"},
                        Case{KeyvaultMode::RawBlock, "/KVRAW2"}}) {
    auto state = std::make_shared<Scripted>();
    const Bytes blob = ut::patternBytes(16384 + 1, 5);
    state->add()->reply(httpResponse("200 OK", blob, false));
    XellClient client(connectorFor(state));
    auto kv = client.getKeyvault(c.mode);
    REQUIRE_OK(kv);
    CHECK_EQ(*kv, blob);
    CHECK(state->scripts[0]->writtenText().find(std::string("GET ") + c.path + " HTTP/1.0") == 0);
  }
}

TEST(XellClient, KeyvaultDefaultsToDecrypted) {
  auto state = std::make_shared<Scripted>();
  state->add()->reply(httpResponse("200 OK", Bytes(10, 0)));
  XellClient client(connectorFor(state));
  auto kv = client.getKeyvault();
  REQUIRE_OK(kv);
  CHECK_EQ(kv->size(), size_t{10});
  CHECK(state->scripts[0]->writtenText().find("GET /KV HTTP/1.0") == 0);
}

TEST(XellClient, KeyvaultWithEmbeddedNulBytesSurvives) {
  auto state = std::make_shared<Scripted>();
  const Bytes blob{0, 0, 1, 0, 0xFF, 0, 0};
  state->add()->reply(httpResponse("200 OK", blob));
  XellClient client(connectorFor(state));
  auto kv = client.getKeyvault(KeyvaultMode::Raw);
  REQUIRE_OK(kv);
  CHECK_EQ(*kv, blob);
}

TEST(XellDump, NoContentLengthDumpsKeepThePartialFinalChunk) {
  for (size_t size : {size_t{40000}, size_t{16384 + 1}}) {
    ut::TempDir dir;
    REQUIRE(dir.ok());
    const Bytes dump = ut::patternBytes(size, static_cast<uint32_t>(size));
    auto state = std::make_shared<Scripted>();
    state->add()->reply(ut::bytesOf("HTTP/1.0 200 OK\r\nConnection: close\r\n\r\n")).replyChunked(dump, 16384);
    XellClient client(connectorFor(state));

    std::vector<std::pair<size_t, size_t>> progress;
    const auto out = dir.file("flash.bin");
    REQUIRE_OK(client.dumpFlash(out, [&](size_t got, size_t total) { progress.emplace_back(got, total); }));

    auto onDisk = ut::readFile(out);
    REQUIRE(onDisk.has_value());
    CHECK_EQ(onDisk->size(), size);
    CHECK_EQ(*onDisk, dump);
    REQUIRE(!progress.empty());
    CHECK_EQ(progress.back(), (std::pair<size_t, size_t>{size, 0}));
    CHECK_EQ(dir.entries(), std::vector<std::string>{"flash.bin"});
    CHECK(state->scripts[0]->writtenText().find("GET /FLASH HTTP/1.0") == 0);
  }
}

TEST(XellDump, ContentLengthDumpReportsTotal) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const Bytes dump = ut::patternBytes(50000, 2);
  auto state = std::make_shared<Scripted>();
  state->add()->reply(httpResponse("200 OK", dump)).setMaxRead(8192);
  XellClient client(connectorFor(state));
  size_t lastTotal = 0;
  REQUIRE_OK(client.dumpFlash(dir.file("flash.bin"), [&](size_t, size_t total) { lastTotal = total; }));
  CHECK_EQ(lastTotal, size_t{50000});
  auto onDisk = ut::readFile(dir.file("flash.bin"));
  REQUIRE(onDisk.has_value());
  CHECK_EQ(*onDisk, dump);
}

TEST(XellDump, TruncatedDumpLeavesNoFile) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto state = std::make_shared<Scripted>();
  state->add()->reply("HTTP/1.0 200 OK\r\nContent-Length: 100000\r\n\r\n").reply(ut::patternBytes(30000));
  XellClient client(connectorFor(state));
  CHECK_ERR(client.dumpFlash(dir.file("flash.bin")), ErrorCode::Disconnected);
  CHECK(dir.entries().empty());
}

TEST(XellDump, TimeoutKeepsPreviousDumpIntact) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const auto out = dir.file("flash.bin");
  const Bytes previous = ut::bytesOf("earlier dump");
  REQUIRE(ut::writeFile(out, previous));
  auto state = std::make_shared<Scripted>();
  state->add()->reply("HTTP/1.0 200 OK\r\n\r\n").reply(ut::patternBytes(500)).replyTimeout();
  XellClient client(connectorFor(state));
  CHECK_ERR(client.dumpFlash(out), ErrorCode::Timeout);
  auto onDisk = ut::readFile(out);
  REQUIRE(onDisk.has_value());
  CHECK_EQ(*onDisk, previous);
  CHECK_EQ(dir.entries(), std::vector<std::string>{"flash.bin"});
}

TEST(XellDump, HttpErrorCreatesNothingAndConnectorFailuresPropagate) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  {
    auto state = std::make_shared<Scripted>();
    state->add()->reply("HTTP/1.0 503 Busy\r\n\r\n");
    XellClient client(connectorFor(state));
    CHECK_ERR(client.dumpFlash(dir.file("a.bin")), ErrorCode::Protocol);
  }
  {
    XellClient client([]() -> Result<net::TransportPtr> { return fail(ErrorCode::ConnectFailed, "down"); });
    CHECK_ERR(client.dumpFlash(dir.file("b.bin")), ErrorCode::ConnectFailed);
  }
  CHECK(dir.entries().empty());
}

TEST(XellDump, ReusingTheClientForTwoDumps) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto state = std::make_shared<Scripted>();
  state->add()->reply("HTTP/1.0 200 OK\r\n\r\n").reply(ut::patternBytes(20000, 1));
  state->add()->reply("HTTP/1.0 200 OK\r\n\r\n").reply(ut::patternBytes(20001, 2));
  XellClient client(connectorFor(state));
  REQUIRE_OK(client.dumpFlash(dir.file("one.bin")));
  REQUIRE_OK(client.dumpFlash(dir.file("two.bin")));
  CHECK_EQ(state->calls, size_t{2});
  CHECK_EQ(ut::readFile(dir.file("one.bin"))->size(), size_t{20000});
  CHECK_EQ(ut::readFile(dir.file("two.bin"))->size(), size_t{20001});
}

TEST(XellForEndpoint, DefaultsToPort80AndOmitsItFromTheHostHeader) {
  auto script = MockScript::create();
  script->reply(okPage(infoPage()));
  net::Endpoint seen;
  net::SchemeTraits traits;
  traits.usesProtocolPort = true;
  net::TransportRegistry::instance().registerScheme("mockxell", [&](const net::Endpoint &e) -> Result<net::TransportPtr> {
    seen = e;
    return net::TransportPtr(script->transport());
  }, traits);

  net::Endpoint endpoint;
  endpoint.scheme = "mockxell";
  endpoint.host = "10.0.0.2";
  auto client = XellClient::forEndpoint(endpoint);
  REQUIRE_OK(client.getInfo());
  CHECK_EQ(seen.port, 80);
  CHECK_EQ(seen.host, std::string("10.0.0.2"));
  CHECK(script->writtenText().find("Host: 10.0.0.2\r\n") != std::string::npos);
  net::TransportRegistry::instance().unregisterScheme("mockxell");
}

TEST(XellForEndpoint, PortlessSchemesKeepPortZero) {
  auto script = MockScript::create();
  script->reply(okPage(infoPage()));
  net::Endpoint seen;
  net::TransportRegistry::instance().registerScheme("mockxserial", [&](const net::Endpoint &e) -> Result<net::TransportPtr> {
    seen = e;
    return net::TransportPtr(script->transport());
  });
  auto endpoint = net::Endpoint::parse("mockxserial:///dev/ttyUSB0");
  REQUIRE_OK(endpoint);
  REQUIRE_OK(XellClient::forEndpoint(*endpoint).getInfo());
  CHECK_EQ(seen.port, 0);
  CHECK_EQ(seen.toString(), std::string("mockxserial:///dev/ttyUSB0"));
  CHECK(script->writtenText().find("Host: /dev/ttyUSB0\r\n") != std::string::npos);
  net::TransportRegistry::instance().unregisterScheme("mockxserial");
}

TEST(XellForEndpoint, NonDefaultPortAppearsInTheHostHeader) {
  auto script = MockScript::create();
  script->reply(okPage(infoPage()));
  net::Endpoint seen;
  net::TransportRegistry::instance().registerScheme("mockxell2", [&](const net::Endpoint &e) -> Result<net::TransportPtr> {
    seen = e;
    return net::TransportPtr(script->transport());
  });
  net::Endpoint endpoint;
  endpoint.scheme = "mockxell2";
  endpoint.host = "10.0.0.2";
  endpoint.port = 8080;
  REQUIRE_OK(XellClient::forEndpoint(endpoint).getInfo());
  CHECK_EQ(seen.port, 8080);
  CHECK(script->writtenText().find("Host: 10.0.0.2:8080\r\n") != std::string::npos);
  net::TransportRegistry::instance().unregisterScheme("mockxell2");
}

TEST(XellForEndpoint, Ipv6HostIsBracketedInTheHostHeader) {
  auto script = MockScript::create();
  script->reply(okPage(infoPage()));
  net::TransportRegistry::instance().registerScheme("mockxell3", [&](const net::Endpoint &) -> Result<net::TransportPtr> {
    return net::TransportPtr(script->transport());
  });
  net::Endpoint endpoint;
  endpoint.scheme = "mockxell3";
  endpoint.host = "fe80::1";
  REQUIRE_OK(XellClient::forEndpoint(endpoint).getInfo());
  CHECK(script->writtenText().find("Host: [fe80::1]\r\n") != std::string::npos);
  net::TransportRegistry::instance().unregisterScheme("mockxell3");
}

TEST(XellForEndpoint, UnregisteredSchemeIsReportedByTheRequest) {
  net::Endpoint endpoint;
  endpoint.scheme = "no-such-transport";
  endpoint.host = "10.0.0.2";
  auto client = XellClient::forEndpoint(endpoint);
  CHECK_ERR(client.getInfo(), ErrorCode::Unsupported);
}
