#include "cli/commands.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <format>

namespace updclient::cli {

namespace {

std::string isoTime(std::chrono::system_clock::time_point time) {
  return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(time));
}

Outcome<void> runDiscover(Context &context) {
  const auto timeout = std::chrono::milliseconds(context.options.discoveryTimeoutMs);
  spdlog::info("Listening for console announcements for up to {} ms...", timeout.count());

  auto found = discovery::DiscoveryRegistry::instance().discoverAll(timeout, false);
  if (!found) {
    return failWith(kExitDiscovery, "DiscoveryFailed", "discovery is unavailable: " + found.error().message);
  }

  nlohmann::json devices = nlohmann::json::array();
  std::string text;
  for (const auto &device : *found) {
    nlohmann::json entry = {{"protocol", device.protocol},
                            {"address", device.address},
                            {"info", device.info},
                            {"last_seen", isoTime(device.lastSeen)}};
    devices.push_back(std::move(entry));
    text += std::format("{}  {}", device.address, device.protocol);
    for (const auto &[key, value] : device.info) text += std::format("  {}={}", key, value);
    text += "\n";
  }

  context.output.result({{"devices", devices}}, text);
  if (found->empty()) {
    spdlog::error("No devices answered within {} ms", timeout.count());
    context.exitCode = kExitDiscovery;
  }
  return {};
}

} // namespace

void registerDiscoverCommand(CLI::App &app, Context &context) {
  auto *discover = app.add_subcommand("discover", "List consoles that announce themselves on the network "
                                                  "(UpdServer broadcasts on UDP port 48)");
  discover->callback([&context] { context.finish(runDiscover(context)); });
}

} // namespace updclient::cli
