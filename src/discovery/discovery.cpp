#include <discovery/discovery.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <iterator>
#include <optional>

namespace updclient::discovery {

IDiscoveryProvider::~IDiscoveryProvider() = default;

DiscoveryRegistry &DiscoveryRegistry::instance() {
  static DiscoveryRegistry registry;
  return registry;
}

void DiscoveryRegistry::add(std::unique_ptr<IDiscoveryProvider> provider) {
  if (!provider) return;
  std::shared_ptr<IDiscoveryProvider> shared(std::move(provider));
  const std::string providerName = shared->name();

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(providers_.begin(), providers_.end(),
                         [&](const auto &existing) { return existing->name() == providerName; });
  if (it != providers_.end()) {
    *it = std::move(shared);
  } else {
    providers_.push_back(std::move(shared));
  }
}

std::vector<std::shared_ptr<IDiscoveryProvider>> DiscoveryRegistry::providers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return providers_;
}

Result<std::vector<DiscoveredDevice>>
DiscoveryRegistry::discoverAll(std::chrono::milliseconds timeout, bool stopAfterFirst) const {
  std::vector<DiscoveredDevice> devices;
  std::optional<Error> firstError;
  bool anySucceeded = false;

  for (const auto &provider : providers()) {
    auto found = provider->discover(timeout, stopAfterFirst);
    if (!found) {
      spdlog::debug("discovery provider '{}' failed: {}", provider->name(), formatError(found.error()));
      if (!firstError) firstError = found.error();
      continue;
    }
    anySucceeded = true;
    devices.insert(devices.end(), std::make_move_iterator(found->begin()),
                   std::make_move_iterator(found->end()));
    if (stopAfterFirst && !devices.empty()) break;
  }

  if (!anySucceeded && firstError) return unexpected<Error>(std::move(*firstError));
  return devices;
}

} // namespace updclient::discovery
