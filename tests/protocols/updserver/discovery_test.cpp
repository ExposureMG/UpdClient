#include "support/fake_datagram_socket.hpp"
#include "support/test_harness.hpp"

#include <updclient/protocols/updserver/discovery.hpp>

#include <chrono>

using namespace updclient;
using namespace updclient::updserver;
using namespace std::chrono_literals;
using ut::Bytes;
using ut::FakeDatagrams;

TEST(UpdServerDiscovery, NameIdentifiesTheProtocol) {
  UpdServerDiscovery discovery;
  CHECK_EQ(discovery.name(), std::string("updserver"));
}

TEST(UpdServerDiscovery, AcceptsAValidAnnouncement) {
  auto fake = FakeDatagrams::create();
  fake->push(ut::announcement(192, 168, 1, 5), "192.168.1.5");
  UpdServerDiscovery discovery(fake->factory());

  auto found = discovery.discover(60ms, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  const auto &device = (*found)[0];
  CHECK_EQ(device.protocol, std::string("updserver"));
  CHECK_EQ(device.address, std::string("192.168.1.5"));
  CHECK_EQ(device.info.at("port"), std::string("49"));
  CHECK_EQ(device.info.at("announcedAddress"), std::string("192.168.1.5"));
  CHECK(device.lastSeen.time_since_epoch().count() != 0);
}

TEST(UpdServerDiscovery, BindsTheAnnouncePortWithReuse) {
  auto fake = FakeDatagrams::create();
  UpdServerDiscovery discovery(fake->factory());
  REQUIRE_OK(discovery.discover(10ms, false));
  CHECK_EQ(fake->bindCalls(), 1);
  CHECK_EQ(fake->boundPort(), 48);
  CHECK(fake->boundWithReuse());
}

TEST(UpdServerDiscovery, CustomAnnouncePort) {
  auto fake = FakeDatagrams::create();
  UpdServerDiscovery discovery(fake->factory(), 4848);
  REQUIRE_OK(discovery.discover(10ms, false));
  CHECK_EQ(fake->boundPort(), 4848);
}

TEST(UpdServerDiscovery, ZeroAnnouncedAddressIsOmitted) {
  auto fake = FakeDatagrams::create();
  fake->push(ut::announcement(0, 0, 0, 0), "10.0.0.9");
  UpdServerDiscovery discovery(fake->factory());
  auto found = discovery.discover(30ms, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK((*found)[0].info.find("announcedAddress") == (*found)[0].info.end());
}

TEST(UpdServerDiscovery, WrongMagicIsIgnored) {
  auto fake = FakeDatagrams::create();
  fake->push(Bytes{'N', 'S', 'v', 'x', 1, 2, 3, 4}, "10.0.0.1");
  fake->push(Bytes{'n', 's', 'v', 'r', 1, 2, 3, 4}, "10.0.0.2");
  fake->push(Bytes{'r', 'v', 'S', 'N', 1, 2, 3, 4}, "10.0.0.3");
  fake->push(Bytes(8, 0), "10.0.0.4");
  UpdServerDiscovery discovery(fake->factory());
  auto found = discovery.discover(60ms, false);
  REQUIRE_OK(found);
  CHECK(found->empty());
}

TEST(UpdServerDiscovery, WrongSizeIsIgnored) {
  auto fake = FakeDatagrams::create();
  fake->push(Bytes{}, "10.0.0.1");
  fake->push(Bytes{'N', 'S', 'v', 'r'}, "10.0.0.2");
  fake->push(Bytes{'N', 'S', 'v', 'r', 1, 2, 3}, "10.0.0.3");
  fake->push(Bytes{'N', 'S', 'v', 'r', 1, 2, 3, 4, 5}, "10.0.0.4");
  UpdServerDiscovery discovery(fake->factory());
  auto found = discovery.discover(60ms, false);
  REQUIRE_OK(found);
  CHECK(found->empty());
}

TEST(UpdServerDiscovery, GarbageDoesNotHideALaterValidAnnouncement) {
  auto fake = FakeDatagrams::create();
  fake->push(Bytes(100, 0xAA), "10.0.0.1");
  fake->pushNothing();
  fake->push(ut::announcement(10, 0, 0, 7), "10.0.0.7");
  UpdServerDiscovery discovery(fake->factory());
  auto found = discovery.discover(100ms, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ((*found)[0].address, std::string("10.0.0.7"));
}

TEST(UpdServerDiscovery, CollectsDistinctConsoles) {
  auto fake = FakeDatagrams::create();
  fake->push(ut::announcement(10, 0, 0, 1), "10.0.0.1");
  fake->push(ut::announcement(10, 0, 0, 2), "10.0.0.2");
  UpdServerDiscovery discovery(fake->factory());
  auto found = discovery.discover(80ms, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{2});
  CHECK_EQ((*found)[0].address, std::string("10.0.0.1"));
  CHECK_EQ((*found)[1].address, std::string("10.0.0.2"));
}

TEST(UpdServerDiscovery, RepeatedAnnouncementsFromOneSenderAreReportedOnce) {
  auto fake = FakeDatagrams::create();
  for (int i = 0; i < 5; ++i) fake->push(ut::announcement(10, 0, 0, 1), "10.0.0.1");
  UpdServerDiscovery discovery(fake->factory());
  auto found = discovery.discover(80ms, false);
  REQUIRE_OK(found);
  CHECK_EQ(found->size(), size_t{1});
}

TEST(UpdServerDiscovery, StopAfterFirstReturnsEarly) {
  auto fake = FakeDatagrams::create();
  fake->push(ut::announcement(10, 0, 0, 1), "10.0.0.1");
  fake->push(ut::announcement(10, 0, 0, 2), "10.0.0.2");
  UpdServerDiscovery discovery(fake->factory());

  const auto start = std::chrono::steady_clock::now();
  auto found = discovery.discover(30s, true);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ((*found)[0].address, std::string("10.0.0.1"));
  CHECK_EQ(fake->receiveCalls(), 1);
  CHECK_EQ(fake->pendingSteps(), size_t{1});
  CHECK(elapsed < 5s);
}

TEST(UpdServerDiscovery, StopAfterFirstSkipsNoiseButStopsAtTheFirstHit) {
  auto fake = FakeDatagrams::create();
  fake->push(Bytes(3, 1), "10.0.0.9");
  fake->push(ut::announcement(10, 0, 0, 1), "10.0.0.1");
  fake->push(ut::announcement(10, 0, 0, 2), "10.0.0.2");
  UpdServerDiscovery discovery(fake->factory());
  auto found = discovery.discover(30s, true);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ(fake->receiveCalls(), 2);
}

TEST(UpdServerDiscovery, WithoutStopAfterFirstRunsUntilTheTimeout) {
  auto fake = FakeDatagrams::create();
  fake->push(ut::announcement(10, 0, 0, 1), "10.0.0.1");
  UpdServerDiscovery discovery(fake->factory());
  const auto start = std::chrono::steady_clock::now();
  auto found = discovery.discover(120ms, false);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  REQUIRE_OK(found);
  CHECK_EQ(found->size(), size_t{1});
  CHECK(elapsed >= 100ms);
  CHECK(elapsed < 5s);
}

TEST(UpdServerDiscovery, NothingAnnouncedGivesAnEmptySuccess) {
  auto fake = FakeDatagrams::create();
  UpdServerDiscovery discovery(fake->factory());
  auto found = discovery.discover(30ms, false);
  REQUIRE_OK(found);
  CHECK(found->empty());
}

TEST(UpdServerDiscovery, ZeroTimeoutStillAttemptsOneReceive) {
  auto fake = FakeDatagrams::create();
  fake->push(ut::announcement(10, 0, 0, 1), "10.0.0.1");
  UpdServerDiscovery discovery(fake->factory());
  auto found = discovery.discover(0ms, false);
  REQUIRE_OK(found);
  CHECK_EQ(found->size(), size_t{1});
  CHECK_EQ(fake->receiveCalls(), 1);
}

TEST(UpdServerDiscovery, BindErrorSurfacesAsAnError) {
  auto fake = FakeDatagrams::create();
  fake->failBind(makeError(ErrorCode::Io, "Permission denied", 13));
  UpdServerDiscovery discovery(fake->factory());
  const auto found = discovery.discover(50ms, false);
  REQUIRE_ERR(found, ErrorCode::Io);
  CHECK_EQ(found.error().sysError, 13);
  CHECK(found.error().message.find("48") != std::string::npos);
  CHECK(found.error().message.find("Permission denied") != std::string::npos);
  CHECK_EQ(fake->receiveCalls(), 0);
}

TEST(UpdServerDiscovery, BindErrorOnAnUnprivilegedPortGivesNoPrivilegeHint) {
  auto fake = FakeDatagrams::create();
  fake->failBind(makeError(ErrorCode::Io, "Address already in use", 98));
  UpdServerDiscovery discovery(fake->factory(), 40000);
  const auto found = discovery.discover(50ms, false);
  REQUIRE_ERR(found, ErrorCode::Io);
  CHECK(found.error().message.find("privileged") == std::string::npos);
  CHECK(found.error().message.find("40000") != std::string::npos);
}

TEST(UpdServerDiscovery, ReceiveErrorBeforeAnyDeviceIsReported) {
  auto fake = FakeDatagrams::create();
  fake->pushError(makeError(ErrorCode::Io, "network down", 100));
  UpdServerDiscovery discovery(fake->factory());
  const auto found = discovery.discover(50ms, false);
  REQUIRE_ERR(found, ErrorCode::Io);
  CHECK_EQ(found.error().sysError, 100);
}

TEST(UpdServerDiscovery, ReceiveErrorAfterADeviceKeepsTheResult) {
  auto fake = FakeDatagrams::create();
  fake->push(ut::announcement(10, 0, 0, 1), "10.0.0.1");
  fake->pushError(makeError(ErrorCode::Io, "network down"));
  UpdServerDiscovery discovery(fake->factory());
  auto found = discovery.discover(200ms, false);
  REQUIRE_OK(found);
  CHECK_EQ(found->size(), size_t{1});
}

TEST(UpdServerDiscovery, FactoryReturningNoSocketIsAnError) {
  UpdServerDiscovery discovery([]() -> std::unique_ptr<net::IDatagramSocket> { return nullptr; });
  CHECK_ERR(discovery.discover(10ms, false), ErrorCode::Unknown);
}

TEST(UpdServerDiscovery, EachDiscoverCallUsesAFreshSocket) {
  auto fake = FakeDatagrams::create();
  UpdServerDiscovery discovery(fake->factory());
  REQUIRE_OK(discovery.discover(5ms, false));
  REQUIRE_OK(discovery.discover(5ms, false));
  CHECK_EQ(fake->socketsCreated(), 2);
  CHECK_EQ(fake->bindCalls(), 2);
}

TEST(UpdServerDiscovery, WorksThroughTheProviderInterface) {
  auto fake = FakeDatagrams::create();
  fake->push(ut::announcement(10, 0, 0, 1), "10.0.0.1");
  discovery::DiscoveryRegistry registry;
  registry.add(std::make_unique<UpdServerDiscovery>(fake->factory()));
  auto found = registry.discoverAll(50ms);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ((*found)[0].protocol, std::string("updserver"));
}

TEST(UpdServerDiscovery, RegisterHelperAddsTheProviderToTheGivenRegistry) {
  discovery::DiscoveryRegistry registry;
  registerUpdServerDiscovery(registry);
  registerUpdServerDiscovery(registry);
  const auto providers = registry.providers();
  REQUIRE_EQ(providers.size(), size_t{1});
  CHECK_EQ(providers[0]->name(), std::string("updserver"));
}

TEST(UpdServerDiscovery, EffectivelyInfiniteTimeoutStillWaitsForAnnouncement) {
  auto fake = FakeDatagrams::create();
  fake->pushNothing().pushNothing().push(ut::announcement(192, 168, 1, 7), "192.168.1.7");
  UpdServerDiscovery discovery(fake->factory());

  auto found = discovery.discover(std::chrono::milliseconds::max(), true);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ((*found)[0].address, std::string("192.168.1.7"));
}
