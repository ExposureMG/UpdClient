#include <protocols/xell/discovery.hpp>

#include <net/http_lite.hpp>
#include <net/transport_registry.hpp>
#include <protocols/xell/client.hpp>

#include "net/deadline.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>

namespace updclient::xell {

namespace {

constexpr size_t kProbeBodyBytes = 16 * 1024;

std::string lowered(std::string_view text) {
  std::string out(text);
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool isUnreachable(ErrorCode code) {
  return code == ErrorCode::ConnectFailed || code == ErrorCode::Timeout || code == ErrorCode::Disconnected ||
         code == ErrorCode::Io;
}

XellProbeResult negative(std::string note) {
  XellProbeResult result;
  result.note = std::move(note);
  return result;
}

std::string hostHeaderFor(const net::Endpoint &endpoint) {
  std::string host = endpoint.host.find(':') == std::string::npos ? endpoint.host : "[" + endpoint.host + "]";
  if (endpoint.port != 0 && endpoint.port != kXellHttpPort) host += ":" + std::to_string(endpoint.port);
  return host;
}

} // namespace

XellProbeResult XellDiscovery::probeTransport(net::ITransport &transport, std::string_view hostHeader) {
  net::HttpExchange exchange(transport);

  net::HttpRequest request{std::string(hostHeader), "/"};
  request.userAgent = "UpdClient-Probe";
  if (auto sent = exchange.sendGet(request); !sent) return negative("request failed: " + formatError(sent.error()));

  auto head = exchange.readHead();
  if (!head) return negative("no HTTP response: " + formatError(head.error()));

  XellProbeResult result;
  result.httpResponse = true;
  result.statusCode = head->statusCode;

  if (const std::string *server = head->header("server"); server && lowered(*server).find("xell") != std::string::npos) {
    result.isXell = true;
    result.note = "Server header names XeLL";
    return result;
  }

  if (!head->isSuccess()) {
    result.note = "HTTP " + std::to_string(head->statusCode) + " without XeLL marker";
    return result;
  }

  auto body = exchange.readBodyPrefix(*head, kProbeBodyBytes);
  if (!body) {
    result.note = "HTTP " + std::to_string(head->statusCode) + ", body unreadable: " + formatError(body.error());
    return result;
  }

  const std::string_view page(reinterpret_cast<const char *>(body->data()), body->size());
  const std::string text = lowered(page);
  if (text.find("xell") != std::string::npos) {
    result.isXell = true;
    result.note = "index page mentions XeLL";
  } else if (text.find("cpu") != std::string::npos && parseXellInfo(page).cpuKeyValid) {
    result.isXell = true;
    result.note = "index page lists a CPU key";
  } else {
    result.note = "HTTP " + std::to_string(head->statusCode) + " without XeLL marker";
  }
  return result;
}

Result<XellProbeResult> XellDiscovery::probeDetailed(const net::Endpoint &endpoint, const Connector &connector) {
  auto &registry = net::TransportRegistry::instance();
  const net::Endpoint target = registry.withDefaultPort(endpoint, kXellHttpPort);

  auto transport = connector ? connector(target) : registry.connect(target);
  if (!transport) {
    if (isUnreachable(transport.error().code)) return negative("unreachable: " + formatError(transport.error()));
    return unexpected<Error>(transport.error());
  }
  if (!*transport) return negative("connector returned no transport");

  XellProbeResult result = probeTransport(**transport, hostHeaderFor(target));
  if (result.isXell) spdlog::debug("detected XeLL HTTPD at {} ({})", (*transport)->describe(), result.note);
  return result;
}

Result<bool> XellDiscovery::probe(const net::Endpoint &endpoint, const Connector &connector) {
  auto result = probeDetailed(endpoint, connector);
  if (!result) return unexpected<Error>(result.error());
  return result->isXell;
}

XellProbeProvider::XellProbeProvider(std::vector<net::Endpoint> candidates, XellDiscovery::Connector connector)
    : candidates_(std::move(candidates)), connector_(std::move(connector)) {}

std::string XellProbeProvider::name() const {
  return "xell";
}

Result<std::vector<discovery::DiscoveredDevice>> XellProbeProvider::discover(std::chrono::milliseconds timeout,
                                                                             bool stopAfterFirst) {
  std::vector<discovery::DiscoveredDevice> devices;
  std::optional<Error> firstError;
  size_t failures = 0;

  const auto deadline = net::deadlineAfter(timeout);
  for (const auto &candidate : candidates_) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) break;

    net::Endpoint target = candidate;
    target.timeout = std::min(target.timeout, remaining);

    auto result = XellDiscovery::probeDetailed(target, connector_);
    if (!result) {
      ++failures;
      if (!firstError) firstError = result.error();
      continue;
    }
    if (!result->isXell) continue;

    discovery::DiscoveredDevice device;
    device.protocol = "xell";
    device.address = candidate.host;
    device.info["endpoint"] = candidate.toString();
    const net::Endpoint resolved = net::TransportRegistry::instance().withDefaultPort(candidate, kXellHttpPort);
    if (resolved.port != 0) device.info["port"] = std::to_string(resolved.port);
    device.info["evidence"] = result->note;
    device.lastSeen = std::chrono::system_clock::now();
    devices.push_back(std::move(device));
    if (stopAfterFirst) break;
  }

  if (devices.empty() && firstError && failures == candidates_.size()) return unexpected<Error>(*firstError);
  return devices;
}

} // namespace updclient::xell
