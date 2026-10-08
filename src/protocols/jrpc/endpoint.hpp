#pragma once

// Private to the library.

#include <net/endpoint.hpp>
#include <net/transport_registry.hpp>
#include <protocols/jrpc/protocol.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace updclient::jrpc {

// The endpoint with the JRPC port filled in when it has none: jrpc:// and a bare host
// (no scheme) get 1409; any other scheme gets what the TransportRegistry gives it, with
// 1409 as the protocol port (so tcp:// gets 1409 and a registered "jrpc" scheme its own
// default). A non-zero port is kept.
inline net::Endpoint withDefaultJrpcPort(const net::Endpoint &endpoint) {
  if (endpoint.port != 0) return endpoint;
  std::string scheme = endpoint.scheme;
  std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (scheme.empty() || scheme == "jrpc") {
    net::Endpoint result = endpoint;
    result.port = kJrpcPort;
    return result;
  }
  return net::TransportRegistry::instance().withDefaultPort(endpoint, kJrpcPort);
}

} // namespace updclient::jrpc
