#include "support/mock_transport.hpp"
#include "support/test_harness.hpp"

#include <net/tcp_transport.hpp>
#include <net/transport_registry.hpp>
#include <updclient.hpp>

#include <algorithm>
#include <atomic>
#include <thread>

using namespace updclient;
using updclient::net::Endpoint;
using updclient::net::TransportRegistry;

namespace {

Endpoint endpointFor(const std::string &scheme, const std::string &host = "device", uint16_t port = 1234) {
  Endpoint e;
  e.scheme = scheme;
  e.host = host;
  e.port = port;
  return e;
}

} // namespace

TEST(TransportRegistry, ConnectsThroughRegisteredScheme) {
  TransportRegistry registry;
  auto script = ut::MockScript::create();
  Endpoint seen;
  registry.registerScheme("fake", [&](const Endpoint &e) -> Result<net::TransportPtr> {
    seen = e;
    return net::TransportPtr(script->transport());
  });

  auto transport = registry.connect(endpointFor("fake", "dev0", 77));
  REQUIRE_OK(transport);
  REQUIRE(*transport != nullptr);
  CHECK(transport.value()->isOpen());
  CHECK_EQ(transport.value()->describe(), std::string("mock"));
  CHECK_EQ(seen.scheme, std::string("fake"));
  CHECK_EQ(seen.host, std::string("dev0"));
  CHECK_EQ(seen.port, 77);

  const uint8_t byte = 0x42;
  REQUIRE_OK(transport.value()->writeAll(std::span<const uint8_t>(&byte, 1)));
  CHECK_EQ(script->written(), ut::Bytes{0x42});
}

TEST(TransportRegistry, UnknownSchemeIsUnsupported) {
  TransportRegistry registry;
  const auto r = registry.connect(endpointFor("nosuch"));
  REQUIRE_ERR(r, ErrorCode::Unsupported);
  CHECK(r.error().message.find("nosuch") != std::string::npos);
}

TEST(TransportRegistry, EmptyRegistryRejectsEverything) {
  TransportRegistry registry;
  CHECK(registry.schemes().empty());
  CHECK_ERR(registry.connect(endpointFor("tcp")), ErrorCode::Unsupported);
}

TEST(TransportRegistry, SchemesAreCaseInsensitive) {
  TransportRegistry registry;
  registry.registerScheme("FaKe", [](const Endpoint &) -> Result<net::TransportPtr> {
    return net::TransportPtr(ut::MockScript::create()->transport());
  });
  CHECK_OK(registry.connect(endpointFor("fake")));
  CHECK_OK(registry.connect(endpointFor("FAKE")));
  const auto schemes = registry.schemes();
  REQUIRE_EQ(schemes.size(), size_t{1});
  CHECK_EQ(schemes[0], std::string("fake"));
}

TEST(TransportRegistry, ReRegisteringReplaces) {
  TransportRegistry registry;
  int which = 0;
  registry.registerScheme("fake", [&](const Endpoint &) -> Result<net::TransportPtr> {
    which = 1;
    return net::TransportPtr(ut::MockScript::create()->transport());
  });
  registry.registerScheme("fake", [&](const Endpoint &) -> Result<net::TransportPtr> {
    which = 2;
    return net::TransportPtr(ut::MockScript::create()->transport());
  });
  CHECK_OK(registry.connect(endpointFor("fake")));
  CHECK_EQ(which, 2);
  CHECK_EQ(registry.schemes().size(), size_t{1});
}

TEST(TransportRegistry, UnregisterReportsWhetherSomethingWasRemoved) {
  TransportRegistry registry;
  registry.registerScheme("fake", [](const Endpoint &) -> Result<net::TransportPtr> {
    return net::TransportPtr(ut::MockScript::create()->transport());
  });
  CHECK(registry.unregisterScheme("FAKE"));
  CHECK(!registry.unregisterScheme("fake"));
  CHECK_ERR(registry.connect(endpointFor("fake")), ErrorCode::Unsupported);
}

TEST(TransportRegistry, ConnectorErrorsPropagateUnchanged) {
  TransportRegistry registry;
  registry.registerScheme("flaky", [](const Endpoint &) -> Result<net::TransportPtr> {
    return fail(ErrorCode::ConnectFailed, "no route", 113);
  });
  const auto r = registry.connect(endpointFor("flaky"));
  REQUIRE_ERR(r, ErrorCode::ConnectFailed);
  CHECK_EQ(r.error().message, std::string("no route"));
  CHECK_EQ(r.error().sysError, 113);
}

TEST(TransportRegistry, SchemesListsEverything) {
  TransportRegistry registry;
  for (const char *scheme : {"serial", "tcp", "usb"}) {
    registry.registerScheme(scheme, [](const Endpoint &) -> Result<net::TransportPtr> {
      return net::TransportPtr(ut::MockScript::create()->transport());
    });
  }
  auto schemes = registry.schemes();
  std::sort(schemes.begin(), schemes.end());
  CHECK_EQ(schemes, (std::vector<std::string>{"serial", "tcp", "usb"}));
}

TEST(TransportRegistry, WithDefaultPortKeepsAnExplicitPort) {
  TransportRegistry registry;
  net::SchemeTraits traits;
  traits.defaultPort = 443;
  registry.registerScheme("tls", [](const Endpoint &) -> Result<net::TransportPtr> {
    return fail(ErrorCode::Unknown, "unused");
  }, traits);
  CHECK_EQ(registry.withDefaultPort(endpointFor("tls", "h", 8443), 80).port, 8443);
  CHECK_EQ(registry.withDefaultPort(endpointFor("tcp", "h", 99), 80).port, 99);
}

TEST(TransportRegistry, WithDefaultPortFollowsTheSchemeTraits) {
  TransportRegistry registry;
  const auto unused = [](const Endpoint &) -> Result<net::TransportPtr> { return fail(ErrorCode::Unknown, "unused"); };
  net::SchemeTraits fixed;
  fixed.defaultPort = 443;
  fixed.usesProtocolPort = true;
  net::SchemeTraits protocol;
  protocol.usesProtocolPort = true;
  registry.registerScheme("tls", unused, fixed);
  registry.registerScheme("raw", unused, protocol);
  registry.registerScheme("serial", unused);

  CHECK_EQ(registry.withDefaultPort(endpointFor("tls", "h", 0), 49).port, 443);
  CHECK_EQ(registry.withDefaultPort(endpointFor("RAW", "h", 0), 49).port, 49);
  CHECK_EQ(registry.withDefaultPort(endpointFor("serial", "/dev/ttyUSB0", 0), 49).port, 0);
  CHECK_EQ(registry.withDefaultPort(endpointFor("unregistered", "h", 0), 49).port, 0);
}

TEST(TransportRegistry, WithDefaultPortTreatsTcpAsProtocolPortEvenWhenUnregistered) {
  TransportRegistry registry;
  CHECK_EQ(registry.withDefaultPort(endpointFor("tcp", "h", 0), 80).port, 80);
  CHECK_EQ(registry.withDefaultPort(endpointFor("TCP", "h", 0), 49).port, 49);
}

TEST(TransportRegistry, InstanceIsProcessWide) {
  CHECK(&TransportRegistry::instance() == &TransportRegistry::instance());
}

TEST(TransportRegistry, RegisterBuiltinsAddsTcpAndIsIdempotent) {
  registerBuiltins();
  registerBuiltins();
  const auto schemes = TransportRegistry::instance().schemes();
  CHECK_EQ(std::count(schemes.begin(), schemes.end(), std::string("tcp")), 1);
}

TEST(TransportRegistry, ConcurrentRegisterAndConnect) {
  TransportRegistry registry;
  registry.registerScheme("base", [](const Endpoint &) -> Result<net::TransportPtr> {
    return net::TransportPtr(ut::MockScript::create()->transport());
  });

  std::atomic<int> connected{0};
  std::atomic<int> failed{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 200; ++i) {
        const std::string scheme = "s" + std::to_string(t) + "_" + std::to_string(i % 5);
        registry.registerScheme(scheme, [](const Endpoint &) -> Result<net::TransportPtr> {
          return net::TransportPtr(ut::MockScript::create()->transport());
        });
        if (registry.connect(endpointFor(scheme))) ++connected;
        else ++failed;
        if (registry.connect(endpointFor("base"))) ++connected;
        else ++failed;
        (void)registry.schemes();
      }
    });
  }
  for (auto &thread : threads) thread.join();
  CHECK_EQ(failed.load(), 0);
  CHECK_EQ(connected.load(), 4 * 200 * 2);
}
