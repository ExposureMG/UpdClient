#include "cli/commands.hpp"
#include "cli/interrupt.hpp"
#include "cli/output.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <format>
#include <memory>
#include <stop_token>

namespace updclient::cli {

namespace {

std::string isoTime(std::chrono::system_clock::time_point time) {
  return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(time));
}

Outcome<void> runDiscover(Context &context, const std::string &protocol) {
  const auto timeout = std::chrono::milliseconds(context.options.discoveryTimeoutMs);
  const bool updserver = protocol != "xbdm";
  const bool xbdmToo = protocol != "updserver";
  spdlog::info("Listening for consoles for up to {} ms{}...", timeout.count(),
               updserver && xbdmToo ? " per protocol" : "");

  std::vector<discovery::DiscoveredDevice> found;
  std::vector<std::string> failures;
  // Ctrl-C ends the search; what was found so far is still printed.
  std::stop_source stop;
  {
    InterruptScope interrupt([&stop] { stop.request_stop(); });
    if (updserver) {
      auto announced = discovery::DiscoveryRegistry::instance().discoverAll(timeout, false, stop.get_token());
      if (announced) found = std::move(*announced);
      else failures.push_back("UpdServer: " + announced.error().message);
    }
    if (xbdmToo && !stop.stop_requested()) {
      // Not in the process-wide registry: UpdServer auto-discovery would wait for it too.
      xbdm::XbdmDiscovery provider;
      auto answered = provider.discover(timeout, false, stop.get_token());
      if (answered) found.insert(found.end(), answered->begin(), answered->end());
      else failures.push_back("XBDM: " + answered.error().message);
    }
  }
  const bool cancelled = stop.stop_requested();
  if (!cancelled && found.empty() && !failures.empty() &&
      failures.size() == static_cast<size_t>(updserver) + xbdmToo) {
    std::string reasons;
    for (const auto &f : failures) reasons += (reasons.empty() ? "" : "; ") + f;
    return failWith(kExitDiscovery, "DiscoveryFailed", "discovery is unavailable: " + reasons);
  }
  for (const auto &f : failures) spdlog::warn("Discovery failed for {}", f);

  nlohmann::json devices = nlohmann::json::array();
  std::string text;
  for (const auto &device : found) {
    nlohmann::json entry = {{"protocol", device.protocol},
                            {"address", device.address},
                            {"info", device.info},
                            {"last_seen", isoTime(device.lastSeen)}};
    devices.push_back(std::move(entry));
    text += std::format("{}  {}", terminalText(device.address), device.protocol);
    for (const auto &[key, value] : device.info) text += std::format("  {}={}", terminalText(key), terminalText(value));
    text += "\n";
  }

  context.output.result({{"devices", devices}}, text);
  if (cancelled) {
    return failWith(kExitRuntime, "Cancelled",
                    "discovery was interrupted; " + std::to_string(found.size()) + " device(s) found until then");
  }
  if (found.empty()) {
    spdlog::error("No devices answered within {} ms", timeout.count());
    context.exitCode = kExitDiscovery;
  }
  return {};
}

} // namespace

void registerDiscoverCommand(CLI::App &app, Context &context) {
  auto *discover = app.add_subcommand("discover", "List consoles on the network: UpdServer announcements on UDP "
                                                  "port 48 and XBDM name replies on UDP port 730");
  auto protocol = std::make_shared<std::string>("all");
  discover->add_option("--protocol", *protocol, "Which consoles to look for")
      ->check(CLI::IsMember({"all", "updserver", "xbdm"}))
      ->capture_default_str();
  discover->callback([&context, protocol] { context.finish(runDiscover(context, *protocol)); });
}

} // namespace updclient::cli
