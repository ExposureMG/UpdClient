#include "support/test_harness.hpp"

#include <updclient/net/endpoint.hpp>

using namespace updclient;
using updclient::net::Endpoint;
using namespace std::chrono_literals;

TEST(Endpoint, BareIpMeansTcpWithUnspecifiedPort) {
  auto e = Endpoint::parse("192.168.1.5");
  REQUIRE_OK(e);
  CHECK_EQ(e->scheme, std::string("tcp"));
  CHECK_EQ(e->host, std::string("192.168.1.5"));
  CHECK_EQ(e->port, 0);
  CHECK(e->options.empty());
  CHECK_EQ(e->timeout, 5000ms);
}

TEST(Endpoint, BareHostWithPort) {
  auto e = Endpoint::parse("192.168.1.5:49");
  REQUIRE_OK(e);
  CHECK_EQ(e->scheme, std::string("tcp"));
  CHECK_EQ(e->host, std::string("192.168.1.5"));
  CHECK_EQ(e->port, 49);
}

TEST(Endpoint, TcpUriWithPort) {
  auto e = Endpoint::parse("tcp://192.168.1.5:49");
  REQUIRE_OK(e);
  CHECK_EQ(e->scheme, std::string("tcp"));
  CHECK_EQ(e->host, std::string("192.168.1.5"));
  CHECK_EQ(e->port, 49);
  CHECK_EQ(e->toString(), std::string("tcp://192.168.1.5:49"));
}

TEST(Endpoint, TcpUriWithoutPort) {
  auto e = Endpoint::parse("tcp://10.0.0.2");
  REQUIRE_OK(e);
  CHECK_EQ(e->port, 0);
  CHECK_EQ(e->toString(), std::string("tcp://10.0.0.2"));
}

TEST(Endpoint, SchemeIsLowercased) {
  auto e = Endpoint::parse("TCP://10.0.0.2:80");
  REQUIRE_OK(e);
  CHECK_EQ(e->scheme, std::string("tcp"));
}

TEST(Endpoint, PortBoundaries) {
  auto low = Endpoint::parse("tcp://h:1");
  REQUIRE_OK(low);
  CHECK_EQ(low->port, 1);
  auto high = Endpoint::parse("tcp://h:65535");
  REQUIRE_OK(high);
  CHECK_EQ(high->port, 65535);
}

TEST(Endpoint, UnknownSchemesStillParse) {
  auto e = Endpoint::parse("usb://console-1");
  REQUIRE_OK(e);
  CHECK_EQ(e->scheme, std::string("usb"));
  CHECK_EQ(e->host, std::string("console-1"));
}

TEST(Endpoint, SchemeMayContainPlusDashDot) {
  auto e = Endpoint::parse("tls+tcp://h:1");
  REQUIRE_OK(e);
  CHECK_EQ(e->scheme, std::string("tls+tcp"));
}

TEST(Endpoint, OptionsAreCollected) {
  auto e = Endpoint::parse("tcp://h:1?baud=115200&parity=none");
  REQUIRE_OK(e);
  CHECK_EQ(e->options.size(), size_t{2});
  CHECK_EQ(e->options.at("baud"), std::string("115200"));
  CHECK_EQ(e->options.at("parity"), std::string("none"));
}

TEST(Endpoint, TimeoutOptionFillsTimeoutNotOptions) {
  auto e = Endpoint::parse("tcp://h:1?timeout=250&x=y");
  REQUIRE_OK(e);
  CHECK_EQ(e->timeout, 250ms);
  CHECK(e->options.find("timeout") == e->options.end());
  CHECK_EQ(e->options.at("x"), std::string("y"));
}

TEST(Endpoint, ZeroTimeoutIsAllowed) {
  auto e = Endpoint::parse("tcp://h:1?timeout=0");
  REQUIRE_OK(e);
  CHECK_EQ(e->timeout, 0ms);
}

TEST(Endpoint, HugeTimeoutIsCapped) {
  auto e = Endpoint::parse("tcp://h:1?timeout=9223372036854775807");
  REQUIRE_OK(e);
  CHECK_EQ(e->timeout, Endpoint::kMaxTimeout);

  auto beyond = Endpoint::parse("tcp://h:1?timeout=99999999999999999999999");
  REQUIRE_OK(beyond);
  CHECK_EQ(beyond->timeout, Endpoint::kMaxTimeout);

  auto exact = Endpoint::parse("tcp://h:1?timeout=" + std::to_string(Endpoint::kMaxTimeout.count()));
  REQUIRE_OK(exact);
  CHECK_EQ(exact->timeout, Endpoint::kMaxTimeout);

  auto negative = Endpoint::parse("tcp://h:1?timeout=-5");
  CHECK_ERR(negative, ErrorCode::InvalidArgument);
}

TEST(Endpoint, OptionWithoutValueOrEmptyPairs) {
  auto e = Endpoint::parse("tcp://h:1?flag&&a=b&");
  REQUIRE_OK(e);
  CHECK_EQ(e->options.at("flag"), std::string(""));
  CHECK_EQ(e->options.at("a"), std::string("b"));
  CHECK_EQ(e->options.size(), size_t{2});
}

TEST(Endpoint, OptionValueMayContainEquals) {
  auto e = Endpoint::parse("tcp://h:1?token=a=b");
  REQUIRE_OK(e);
  CHECK_EQ(e->options.at("token"), std::string("a=b"));
}

TEST(Endpoint, BareHostWithOptions) {
  auto e = Endpoint::parse("192.168.1.5?timeout=100");
  REQUIRE_OK(e);
  CHECK_EQ(e->scheme, std::string("tcp"));
  CHECK_EQ(e->host, std::string("192.168.1.5"));
  CHECK_EQ(e->timeout, 100ms);
}

TEST(Endpoint, DeviceStylePathIsAHost) {
  auto e = Endpoint::parse("serial:///dev/ttyUSB0?baud=115200");
  REQUIRE_OK(e);
  CHECK_EQ(e->scheme, std::string("serial"));
  CHECK_EQ(e->host, std::string("/dev/ttyUSB0"));
  CHECK_EQ(e->port, 0);
  CHECK_EQ(e->options.at("baud"), std::string("115200"));
}

TEST(Endpoint, BracketedIpv6) {
  auto e = Endpoint::parse("tcp://[::1]:80");
  REQUIRE_OK(e);
  CHECK_EQ(e->host, std::string("::1"));
  CHECK_EQ(e->port, 80);
  CHECK_EQ(e->toString(), std::string("tcp://[::1]:80"));
}

TEST(Endpoint, BracketedIpv6WithoutPort) {
  auto e = Endpoint::parse("tcp://[fe80::1]");
  REQUIRE_OK(e);
  CHECK_EQ(e->host, std::string("fe80::1"));
  CHECK_EQ(e->port, 0);
}

TEST(Endpoint, UnbracketedIpv6IsAllHost) {
  auto e = Endpoint::parse("tcp://::1");
  REQUIRE_OK(e);
  CHECK_EQ(e->host, std::string("::1"));
  CHECK_EQ(e->port, 0);
}

TEST(Endpoint, MalformedBrackets) {
  CHECK_ERR(Endpoint::parse("tcp://[::1"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://[::1]x"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://[::1]:"), ErrorCode::InvalidArgument);
}

TEST(Endpoint, EmptyInputIsRejected) {
  CHECK_ERR(Endpoint::parse(""), ErrorCode::InvalidArgument);
}

TEST(Endpoint, BadSchemeIsRejected) {
  CHECK_ERR(Endpoint::parse("://h:1"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("1tcp://h:1"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tc p://h:1"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp_x://h:1"), ErrorCode::InvalidArgument);
}

TEST(Endpoint, MissingHostIsRejected) {
  CHECK_ERR(Endpoint::parse("tcp://"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://:49"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://?timeout=5"), ErrorCode::InvalidArgument);
}

TEST(Endpoint, BadPortIsRejected) {
  CHECK_ERR(Endpoint::parse("tcp://h:0"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://h:65536"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://h:99999999999"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://h:abc"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://h:"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://h:-1"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://h:+1"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://h:12x"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("192.168.1.5:"), ErrorCode::InvalidArgument);
}

TEST(Endpoint, BadOptionsAreRejected) {
  CHECK_ERR(Endpoint::parse("tcp://h:1?timeout=abc"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://h:1?timeout="), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://h:1?timeout=-5"), ErrorCode::InvalidArgument);
  CHECK_ERR(Endpoint::parse("tcp://h:1?=x"), ErrorCode::InvalidArgument);
}

TEST(Endpoint, ToStringOmitsDefaults) {
  Endpoint e;
  e.scheme = "tcp";
  e.host = "1.2.3.4";
  CHECK_EQ(e.toString(), std::string("tcp://1.2.3.4"));
}

TEST(Endpoint, ToStringEmitsSortedOptionsThenTimeout) {
  Endpoint e;
  e.scheme = "serial";
  e.host = "/dev/ttyUSB0";
  e.options["parity"] = "none";
  e.options["baud"] = "115200";
  e.timeout = 750ms;
  CHECK_EQ(e.toString(), std::string("serial:///dev/ttyUSB0?baud=115200&parity=none&timeout=750"));
}

TEST(Endpoint, ToStringTimeoutOnly) {
  Endpoint e;
  e.scheme = "tcp";
  e.host = "h";
  e.port = 7;
  e.timeout = 0ms;
  CHECK_EQ(e.toString(), std::string("tcp://h:7?timeout=0"));
}

TEST(Endpoint, RoundTripThroughToString) {
  const char *inputs[] = {
      "tcp://192.168.1.5:49",
      "192.168.1.5:49",
      "tcp://10.0.0.2",
      "tcp://[::1]:8080?timeout=1500",
      "serial:///dev/ttyUSB0?baud=115200&parity=none",
      "usb://console-1?timeout=0&vid=045e",
  };
  for (const char *input : inputs) {
    auto first = Endpoint::parse(input);
    REQUIRE_OK(first);
    auto second = Endpoint::parse(first->toString());
    REQUIRE_OK(second);
    CHECK_EQ(second->scheme, first->scheme);
    CHECK_EQ(second->host, first->host);
    CHECK_EQ(second->port, first->port);
    CHECK_EQ(second->options, first->options);
    CHECK_EQ(second->timeout, first->timeout);
    CHECK_EQ(second->toString(), first->toString());
  }
}
