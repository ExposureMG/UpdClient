#include <protocols/jrpc/discovery.hpp>

#include "net/deadline.hpp"
#include "protocols/jrpc/endpoint.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace updclient::jrpc {

namespace {

// Everything a probe can meet on the way that says "nothing JRPC here". What is left
// (a bad endpoint, a stop request) is a problem with the candidate or the call.
bool isNotJrpc(ErrorCode code) {
  return code != ErrorCode::InvalidArgument && code != ErrorCode::Cancelled;
}

Result<IdentifyResult> probe(const net::Endpoint &target, const ClientOptions &options,
                             const JrpcProbeProvider::Connector &connector) {
  if (!connector) return identify(target, options);
  auto client = JrpcClient::open([connector, target] { return connector(target); }, options);
  if (!client) return unexpected<Error>(client.error());
  IdentifyResult result;
  result.endpoint = withDefaultJrpcPort(target);
  result.banner = std::string(kBanner);
  client->close();
  return result;
}

} // namespace

JrpcProbeProvider::JrpcProbeProvider(std::vector<net::Endpoint> candidates, ClientOptions options,
                                     Connector connector)
    : candidates_(std::move(candidates)), options_(std::move(options)), connector_(std::move(connector)) {}

std::string JrpcProbeProvider::name() const {
  return "jrpc";
}

Result<std::vector<discovery::DiscoveredDevice>> JrpcProbeProvider::discover(std::chrono::milliseconds timeout,
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
    ClientOptions options = options_;
    // Zero means no bound on the banner; the run's own deadline still applies.
    options.bannerTimeout = options.bannerTimeout.count() == 0 ? remaining : std::min(options.bannerTimeout, remaining);

    auto result = probe(target, options, connector_);
    if (!result) {
      if (isNotJrpc(result.error().code)) continue;
      ++failures;
      if (!firstError) firstError = result.error();
      continue;
    }

    discovery::DiscoveredDevice device;
    device.protocol = "jrpc";
    device.address = candidate.host;
    // From the candidate, not the probe's target: that one carries a timeout cut down to
    // the time left in this run, which would leak into the text as "?timeout=<ms>".
    device.info["endpoint"] = withDefaultJrpcPort(candidate).toString();
    if (result->endpoint.port != 0) device.info["port"] = std::to_string(result->endpoint.port);
    device.info["banner"] = result->banner;
    device.lastSeen = std::chrono::system_clock::now();
    devices.push_back(std::move(device));
    if (stopAfterFirst) break;
  }

  if (devices.empty() && firstError && failures == candidates_.size()) return unexpected<Error>(*firstError);
  return devices;
}

} // namespace updclient::jrpc
