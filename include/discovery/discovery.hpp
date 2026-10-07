#pragma once

#include <core/error.hpp>
#include <core/export.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace updclient::discovery {

struct DiscoveredDevice {
  std::string protocol;
  std::string address;
  std::map<std::string, std::string> info;
  std::chrono::system_clock::time_point lastSeen;
};

class UPDCLIENT_API IDiscoveryProvider {
public:
  IDiscoveryProvider() = default;
  IDiscoveryProvider(const IDiscoveryProvider &) = delete;
  IDiscoveryProvider &operator=(const IDiscoveryProvider &) = delete;
  virtual ~IDiscoveryProvider();

  virtual std::string name() const = 0;
  // With stopAfterFirst the provider returns as soon as one device is found.
  virtual Result<std::vector<DiscoveredDevice>> discover(std::chrono::milliseconds timeout,
                                                         bool stopAfterFirst) = 0;
};

// Thread-safe. instance() is the process-wide registry; independent instances
// can be created for isolation (e.g. in tests).
class UPDCLIENT_API DiscoveryRegistry {
public:
  DiscoveryRegistry() = default;
  DiscoveryRegistry(const DiscoveryRegistry &) = delete;
  DiscoveryRegistry &operator=(const DiscoveryRegistry &) = delete;

  static DiscoveryRegistry &instance();

  // A provider with the same name() replaces the existing one.
  void add(std::unique_ptr<IDiscoveryProvider> provider);
  std::vector<std::shared_ptr<IDiscoveryProvider>> providers() const;

  // Runs the providers one after another, each given the full timeout. A provider
  // failure is skipped; the first failure is returned only if every provider failed.
  // With stopAfterFirst, stops at the first provider that finds a device.
  Result<std::vector<DiscoveredDevice>> discoverAll(std::chrono::milliseconds timeout,
                                                    bool stopAfterFirst = false) const;

private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<IDiscoveryProvider>> providers_;
};

} // namespace updclient::discovery
