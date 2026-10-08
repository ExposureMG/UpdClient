#pragma once

#include <core/error.hpp>
#include <core/export.hpp>
#include <discovery/discovery.hpp>
#include <net/endpoint.hpp>
#include <net/transport.hpp>
#include <protocols/jrpc/client.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace updclient::jrpc {

// JRPC does not announce itself, so it is probed rather than listened for: each
// candidate gets identify(), which reads the banner and sends no command. It never scans
// subnets itself, so the caller decides where to look. Not registered by
// registerBuiltins().
//
// A candidate that refuses the connection, does not answer, closes early, or greets with
// something other than the JRPC banner (a DEBUG line included) is simply not a JRPC
// console: it is left out, not an error. That includes a scheme nobody registered, which
// the registry reports as Unsupported like a DEBUG line. Only a bad candidate
// (InvalidArgument: an empty host, a name that does not resolve) is an error, and
// discover() returns it only when every candidate failed that way. A device is reported
// with protocol "jrpc", the candidate's host as address, and info "endpoint", "port"
// (1409 unless the candidate names another) and "banner".
class UPDCLIENT_API JrpcProbeProvider final : public discovery::IDiscoveryProvider {
public:
  using Connector = std::function<Result<net::TransportPtr>(const net::Endpoint &)>;

  // `options` is used for every probe (bannerTimeout is cut to what remains of the
  // run). An empty connector connects like JrpcClient::connect().
  explicit JrpcProbeProvider(std::vector<net::Endpoint> candidates, ClientOptions options = {},
                             Connector connector = {});

  std::string name() const override;
  // timeout bounds the whole run; each candidate gets at most its own Endpoint::timeout
  // and what remains of it.
  Result<std::vector<discovery::DiscoveredDevice>> discover(std::chrono::milliseconds timeout,
                                                            bool stopAfterFirst) override;

private:
  std::vector<net::Endpoint> candidates_;
  ClientOptions options_;
  Connector connector_;
};

} // namespace updclient::jrpc
