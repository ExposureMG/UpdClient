#include "cli/context.hpp"

#include "cli/interrupt.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <stop_token>

#if defined(_WIN32)
#include <io.h>
#define UPDCLIENT_ISATTY _isatty
#define UPDCLIENT_FILENO _fileno
#else
#include <unistd.h>
#define UPDCLIENT_ISATTY isatty
#define UPDCLIENT_FILENO fileno
#endif

namespace updclient::cli {

namespace {

bool interactive() {
  return UPDCLIENT_ISATTY(UPDCLIENT_FILENO(stdin)) != 0 && UPDCLIENT_ISATTY(UPDCLIENT_FILENO(stderr)) != 0;
}

std::string lowered(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::string joinSchemes(const std::vector<std::string> &schemes) {
  std::string out;
  for (const auto &scheme : schemes) out += (out.empty() ? "" : ", ") + scheme;
  return out;
}

// info["port"] of a discovered device, when it is a valid port number.
std::optional<uint16_t> discoveredPort(const discovery::DiscoveredDevice &device) {
  auto it = device.info.find("port");
  if (it == device.info.end()) return std::nullopt;
  unsigned port = 0;
  const auto [ptr, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), port);
  if (ec != std::errc{} || ptr != it->second.data() + it->second.size() || port == 0 || port > 65535) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(port);
}

Outcome<void> checkSchemeRegistered(const net::Endpoint &endpoint) {
  const auto schemes = net::TransportRegistry::instance().schemes();
  if (std::find(schemes.begin(), schemes.end(), lowered(endpoint.scheme)) != schemes.end()) return {};
  return usageError("no transport registered for scheme '" + endpoint.scheme + "' (available: " +
                    joinSchemes(schemes) + ")");
}

} // namespace

int exitCodeFor(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::InvalidArgument: return kExitUsage;
  default: return kExitRuntime;
  }
}

unexpected<Failure> failWith(int exitCode, std::string code, std::string message) {
  return unexpected<Failure>(Failure{std::move(code), std::move(message), 0, exitCode});
}

unexpected<Failure> usageError(std::string message) {
  return failWith(kExitUsage, "Usage", std::move(message));
}

unexpected<Failure> fromError(const Error &error) {
  Failure failure{errorCodeName(error.code), error.message, error.sysError, exitCodeFor(error.code)};
  if (const auto status = xbdm::consoleStatusCode(error)) {
    failure.sysError = 0;
    failure.consoleStatus = *status;
  }
  return unexpected<Failure>(std::move(failure));
}

void Context::applyGlobals() {
  output.setJson(options.json);
  spdlog::set_level(options.verbose ? spdlog::level::debug : spdlog::level::info);
}

Outcome<net::Endpoint> Context::explicitEndpoint(Service service) const {
  const bool forXell = service == Service::Xell;
  if (!options.target.empty() && !options.ip.empty()) {
    return usageError("--target and --ip are mutually exclusive");
  }
  if (forXell && options.port) {
    return usageError("--port selects the UpdServer port; use --xell-port for xell commands");
  }
  if (!forXell && options.xellPort) {
    return usageError("--xell-port only applies to xell commands; use --port for UpdServer");
  }

  const std::string &spec = options.target.empty() ? options.ip : options.target;
  auto parsed = net::Endpoint::parse(spec);
  if (!parsed) return usageError("invalid target '" + spec + "': " + parsed.error().message);
  net::Endpoint endpoint = std::move(*parsed);
  if (service == Service::Xbdm && spec.find("://") == std::string::npos) endpoint.scheme = "xbdm";

  const std::optional<uint16_t> &selected = forXell ? options.xellPort : options.port;
  if (selected) {
    if (endpoint.port != 0 && endpoint.port != *selected) {
      return usageError("conflicting ports: target says " + std::to_string(endpoint.port) + ", option says " +
                        std::to_string(*selected));
    }
    endpoint.port = *selected;
  }
  const std::string scheme = lowered(endpoint.scheme);
  if (endpoint.port == 0 && service == Service::Xbdm && (scheme == "tcp" || scheme == "xbdm")) {
    endpoint.port = xbdm::kXbdmPort;
  } else if (endpoint.port == 0 && scheme == "tcp") {
    endpoint.port = forXell ? xell::kXellHttpPort : updserver::NANDSVR_PORT;
  }
  if (options.timeoutMs) endpoint.timeout = std::chrono::milliseconds(*options.timeoutMs);

  if (auto registered = checkSchemeRegistered(endpoint); !registered) return unexpected<Failure>(registered.error());
  return endpoint;
}

bool Context::targetsXbdm() const {
  if (options.target.empty()) return false;
  auto parsed = net::Endpoint::parse(options.target);
  return parsed && lowered(parsed->scheme) == "xbdm";
}

Outcome<net::Endpoint> Context::resolveUpdServerEndpoint() const {
  if (targetsXbdm()) {
    return usageError("this is an UpdServer command and the target is an XBDM console (xbdm://); see "
                      "'updclient xbdm --help' for what XBDM supports");
  }
  if (hasExplicitTarget()) return explicitEndpoint(Service::UpdServer);
  if (options.xellPort) {
    return usageError("--xell-port only applies to xell commands; use --port for UpdServer");
  }

  const auto timeout = std::chrono::milliseconds(options.discoveryTimeoutMs);
  spdlog::info("No target specified; looking for an UpdServer console (UDP port {}, up to {} ms)...",
               updserver::ANNC_PORT, timeout.count());
  auto found = discovery::DiscoveryRegistry::instance().discoverAll(timeout, true);
  if (!found) {
    return failWith(kExitDiscovery, "DiscoveryFailed",
                    "automatic discovery is unavailable: " + found.error().message +
                        "; pass --target or --ip to skip discovery");
  }

  for (const auto &device : *found) {
    if (device.protocol != "updserver") continue;
    net::Endpoint endpoint;
    endpoint.scheme = "tcp";
    endpoint.host = device.address;
    endpoint.port = discoveredPort(device).value_or(updserver::NANDSVR_PORT);
    if (options.port) endpoint.port = *options.port;
    if (options.timeoutMs) endpoint.timeout = std::chrono::milliseconds(*options.timeoutMs);
    spdlog::info("Using discovered console at {}", endpoint.toString());
    if (auto registered = checkSchemeRegistered(endpoint); !registered) return unexpected<Failure>(registered.error());
    return endpoint;
  }

  return failWith(kExitDiscovery, "NoDevices",
                  "no UpdServer console answered within " + std::to_string(timeout.count()) +
                      " ms; pass --target or --ip to name one");
}

Outcome<net::Endpoint> Context::resolveXellEndpoint() const {
  if (!hasExplicitTarget()) {
    return usageError("xell commands need --target or --ip: XeLL does not announce itself, so it is never auto-discovered");
  }
  if (targetsXbdm()) return usageError("xell commands talk to XeLL's HTTP server, not to an xbdm:// target");
  return explicitEndpoint(Service::Xell);
}

Outcome<net::Endpoint> Context::resolveXbdmEndpoint() const {
  if (hasExplicitTarget()) return explicitEndpoint(Service::Xbdm);
  if (options.xellPort) return usageError("--xell-port only applies to xell commands");

  const auto timeout = std::chrono::milliseconds(options.discoveryTimeoutMs);
  spdlog::info("No target specified; looking for an XBDM console (UDP port {}, up to {} ms)...", xbdm::kXbdmPort,
               timeout.count());
  xbdm::XbdmDiscovery discovery;
  std::stop_source stop;
  Result<std::vector<discovery::DiscoveredDevice>> found;
  {
    InterruptScope interrupt([&stop] { stop.request_stop(); });
    found = discovery.discover(timeout, true, stop.get_token());
  }
  if (stop.stop_requested()) return failWith(kExitRuntime, "Cancelled", "cancelled while looking for an XBDM console");
  if (!found) {
    return failWith(kExitDiscovery, "DiscoveryFailed",
                    "XBDM discovery is unavailable: " + found.error().message + "; pass --target xbdm://<address>");
  }
  if (found->empty()) {
    return failWith(kExitDiscovery, "NoDevices",
                    "no XBDM console answered within " + std::to_string(timeout.count()) +
                        " ms; pass --target xbdm://<address> (many networks block the broadcast)");
  }
  net::Endpoint endpoint;
  endpoint.scheme = "xbdm";
  endpoint.host = found->front().address;
  endpoint.port = options.port.value_or(discoveredPort(found->front()).value_or(xbdm::kXbdmPort));
  if (options.timeoutMs) endpoint.timeout = std::chrono::milliseconds(*options.timeoutMs);
  const auto name = found->front().info.find("name");
  spdlog::info("Using discovered console {} at {}", name != found->front().info.end() ? name->second : "",
               endpoint.toString());
  if (auto registered = checkSchemeRegistered(endpoint); !registered) return unexpected<Failure>(registered.error());
  return endpoint;
}

Outcome<void> Context::requireConfirmationPossible() const {
  if (options.yes || interactive()) return {};
  return usageError("this command is destructive; pass --yes to run it without an interactive terminal");
}

Outcome<void> Context::confirmDestructive(const std::string &action, const std::string &target) const {
  if (options.yes) return {};
  if (!interactive()) {
    return usageError("this command is destructive; pass --yes to run it without an interactive terminal");
  }
  std::cerr << "WARNING: about to " << action << " on " << target << ".\n"
            << "This changes the console and cannot be undone. Type 'yes' to continue: " << std::flush;
  std::string answer;
  std::getline(std::cin, answer);
  if (lowered(answer) != "yes") {
    return failWith(kExitRuntime, "Aborted", "not confirmed; nothing was sent");
  }
  return {};
}

void Context::finish(const Outcome<void> &outcome) {
  if (outcome) return;
  const Failure &failure = outcome.error();
  spdlog::error("{}: {}{}", failure.code, failure.message,
                failure.sysError != 0 ? " (os error " + std::to_string(failure.sysError) + ")" : "");
  if (failure.keptUpload) {
    spdlog::warn("The upload is kept on the console as {}; rename it with 'xbdm mv'", *failure.keptUpload);
  }
  if (failure.delivery == "unknown") spdlog::warn("The console may have carried the command out; check before repeating it");
  output.error(failure.code, failure.message, failure.sysError, failure.consoleStatus, failure.keptUpload,
               failure.delivery);
  exitCode = failure.exitCode;
}

} // namespace updclient::cli
