#include "support/test_harness.hpp"

#include <discovery/discovery.hpp>
#include <updclient.hpp>

#include <algorithm>
#include <atomic>
#include <stop_token>
#include <thread>

using namespace updclient;
using namespace updclient::discovery;
using namespace std::chrono_literals;

namespace {

class StubProvider final : public IDiscoveryProvider {
public:
  StubProvider(std::string providerName, std::vector<std::string> addresses, std::optional<Error> failure = {})
      : name_(std::move(providerName)), addresses_(std::move(addresses)), failure_(std::move(failure)) {}

  std::string name() const override { return name_; }

  Result<std::vector<DiscoveredDevice>> discover(std::chrono::milliseconds, bool stopAfterFirst) override {
    ++calls;
    lastStopAfterFirst = stopAfterFirst;
    if (failure_) return unexpected<Error>(*failure_);
    std::vector<DiscoveredDevice> out;
    for (const auto &address : addresses_) {
      DiscoveredDevice device;
      device.protocol = name_;
      device.address = address;
      out.push_back(device);
      if (stopAfterFirst) break;
    }
    return out;
  }

  // ConcurrentAddAndEnumerate calls discover() on one provider from several threads.
  std::atomic<int> calls{0};
  std::atomic<bool> lastStopAfterFirst{false};

private:
  std::string name_;
  std::vector<std::string> addresses_;
  std::optional<Error> failure_;
};

} // namespace

TEST(DiscoveryRegistry, StartsEmpty) {
  DiscoveryRegistry registry;
  CHECK(registry.providers().empty());
  auto found = registry.discoverAll(10ms);
  REQUIRE_OK(found);
  CHECK(found->empty());
}

TEST(DiscoveryRegistry, AggregatesProvidersInOrder) {
  DiscoveryRegistry registry;
  registry.add(std::make_unique<StubProvider>("a", std::vector<std::string>{"1.1.1.1", "1.1.1.2"}));
  registry.add(std::make_unique<StubProvider>("b", std::vector<std::string>{"2.2.2.2"}));
  auto found = registry.discoverAll(10ms);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{3});
  CHECK_EQ((*found)[0].address, std::string("1.1.1.1"));
  CHECK_EQ((*found)[1].address, std::string("1.1.1.2"));
  CHECK_EQ((*found)[2].address, std::string("2.2.2.2"));
  CHECK_EQ((*found)[2].protocol, std::string("b"));
}

TEST(DiscoveryRegistry, SameNameReplacesTheProvider) {
  DiscoveryRegistry registry;
  registry.add(std::make_unique<StubProvider>("a", std::vector<std::string>{"old"}));
  registry.add(std::make_unique<StubProvider>("a", std::vector<std::string>{"new"}));
  REQUIRE_EQ(registry.providers().size(), size_t{1});
  auto found = registry.discoverAll(10ms);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ((*found)[0].address, std::string("new"));
}

TEST(DiscoveryRegistry, NullProviderIsIgnored) {
  DiscoveryRegistry registry;
  registry.add(nullptr);
  CHECK(registry.providers().empty());
}

TEST(DiscoveryRegistry, FailingProviderIsSkippedWhenAnotherSucceeds) {
  DiscoveryRegistry registry;
  registry.add(std::make_unique<StubProvider>("bad", std::vector<std::string>{}, makeError(ErrorCode::Io, "boom")));
  registry.add(std::make_unique<StubProvider>("good", std::vector<std::string>{"3.3.3.3"}));
  auto found = registry.discoverAll(10ms);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ((*found)[0].address, std::string("3.3.3.3"));
}

TEST(DiscoveryRegistry, AllFailingReturnsTheFirstError) {
  DiscoveryRegistry registry;
  registry.add(std::make_unique<StubProvider>("a", std::vector<std::string>{}, makeError(ErrorCode::Timeout, "first")));
  registry.add(std::make_unique<StubProvider>("b", std::vector<std::string>{}, makeError(ErrorCode::Io, "second")));
  const auto found = registry.discoverAll(10ms);
  REQUIRE_ERR(found, ErrorCode::Timeout);
  CHECK_EQ(found.error().message, std::string("first"));
}

TEST(DiscoveryRegistry, SuccessWithNoDevicesBeatsFailure) {
  DiscoveryRegistry registry;
  registry.add(std::make_unique<StubProvider>("bad", std::vector<std::string>{}, makeError(ErrorCode::Io, "boom")));
  registry.add(std::make_unique<StubProvider>("quiet", std::vector<std::string>{}));
  auto found = registry.discoverAll(10ms);
  REQUIRE_OK(found);
  CHECK(found->empty());
}

TEST(DiscoveryRegistry, StopAfterFirstStopsAtTheFirstProviderWithADevice) {
  DiscoveryRegistry registry;
  auto first = std::make_unique<StubProvider>("a", std::vector<std::string>{"1.1.1.1", "1.1.1.2"});
  auto second = std::make_unique<StubProvider>("b", std::vector<std::string>{"2.2.2.2"});
  auto *firstRaw = first.get();
  auto *secondRaw = second.get();
  registry.add(std::move(first));
  registry.add(std::move(second));

  auto found = registry.discoverAll(10ms, true);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ(firstRaw->calls.load(), 1);
  CHECK(firstRaw->lastStopAfterFirst);
  CHECK_EQ(secondRaw->calls.load(), 0);
}

TEST(DiscoveryRegistry, StopAfterFirstContinuesPastEmptyProviders) {
  DiscoveryRegistry registry;
  registry.add(std::make_unique<StubProvider>("a", std::vector<std::string>{}));
  registry.add(std::make_unique<StubProvider>("b", std::vector<std::string>{"2.2.2.2"}));
  auto found = registry.discoverAll(10ms, true);
  REQUIRE_OK(found);
  CHECK_EQ(found->size(), size_t{1});
}

TEST(DiscoveryRegistry, InstanceIsProcessWideAndBuiltinsAreRegisteredOnce) {
  CHECK(&DiscoveryRegistry::instance() == &DiscoveryRegistry::instance());
  registerBuiltins();
  registerBuiltins();
  const auto providers = DiscoveryRegistry::instance().providers();
  const auto count = std::count_if(providers.begin(), providers.end(),
                                   [](const auto &p) { return p->name() == "updserver"; });
  CHECK_EQ(count, 1);
}

TEST(DiscoveryRegistry, ConcurrentAddAndEnumerate) {
  DiscoveryRegistry registry;
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < 100; ++i) {
        registry.add(std::make_unique<StubProvider>("p" + std::to_string(t) + "_" + std::to_string(i % 3),
                                                    std::vector<std::string>{"x"}));
        if (!registry.discoverAll(1ms)) ++failures;
      }
    });
  }
  for (auto &thread : threads) thread.join();
  CHECK_EQ(failures.load(), 0);
  CHECK_EQ(registry.providers().size(), size_t{12});
}

TEST(DiscoveryRegistry, AStopSkipsTheRemainingProviders) {
  DiscoveryRegistry registry;
  std::stop_source source;
  auto first = std::make_unique<StubProvider>("a", std::vector<std::string>{"1.1.1.1"});
  auto second = std::make_unique<StubProvider>("b", std::vector<std::string>{"2.2.2.2"});
  auto *a = first.get();
  auto *b = second.get();
  registry.add(std::move(first));
  registry.add(std::move(second));

  source.request_stop();
  auto none = registry.discoverAll(10ms, false, source.get_token());
  REQUIRE_OK(none);
  CHECK(none->empty());
  CHECK_EQ(a->calls.load(), 0);
  CHECK_EQ(b->calls.load(), 0);

  auto all = registry.discoverAll(10ms, false, std::stop_token());
  REQUIRE_OK(all);
  CHECK_EQ(all->size(), size_t{2});
}
