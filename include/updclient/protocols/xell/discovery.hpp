#pragma once

#include <updclient/core/error.hpp>
#include <updclient/core/export.hpp>
#include <updclient/discovery/discovery.hpp>
#include <updclient/net/endpoint.hpp>
#include <updclient/net/transport.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace updclient::xell {

struct XellProbeResult {
  bool isXell = false;
  // A well-formed HTTP status line came back, whether or not it was XeLL.
  bool httpResponse = false;
  int statusCode = 0;
  // Why the verdict was reached: the marker that identified XeLL, or what was
  // missing ("HTTP 200 without XeLL marker", "connection refused", ...).
  std::string note;
};

// XeLL does not announce itself, so it is probed rather than listened for.
// XeLL is recognised by content, never by "200 OK" alone: a Server header or an
// index page that mentions XeLL, or an index page that lists a CPU key.
class UPDCLIENT_API XellDiscovery {
public:
  using Connector = std::function<Result<net::TransportPtr>(const net::Endpoint &)>;

  // Port 0 selects 80; an empty connector uses the TransportRegistry. Endpoints
  // that cannot be reached or do not speak HTTP give isXell == false, not an
  // error; only problems with the request itself (unknown scheme, bad argument)
  // are errors.
  static Result<XellProbeResult> probeDetailed(const net::Endpoint &endpoint, const Connector &connector = {});
  static Result<bool> probe(const net::Endpoint &endpoint, const Connector &connector = {});

  // Probes an already open transport. Every failure becomes a negative result.
  static XellProbeResult probeTransport(net::ITransport &transport, std::string_view hostHeader);
};

// Probes a fixed list of candidate endpoints; it never scans subnets itself, so
// the caller decides where to look. Not registered by registerBuiltins().
class UPDCLIENT_API XellProbeProvider final : public discovery::IDiscoveryProvider {
public:
  explicit XellProbeProvider(std::vector<net::Endpoint> candidates, XellDiscovery::Connector connector = {});

  std::string name() const override;
  // timeout bounds the whole run; each candidate gets at most its own
  // Endpoint::timeout and what remains of it.
  Result<std::vector<discovery::DiscoveredDevice>> discover(std::chrono::milliseconds timeout,
                                                            bool stopAfterFirst) override;

private:
  std::vector<net::Endpoint> candidates_;
  XellDiscovery::Connector connector_;
};

} // namespace updclient::xell
