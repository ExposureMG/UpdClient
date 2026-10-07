#pragma once

#include <core/error.hpp>
#include <core/export.hpp>
#include <net/endpoint.hpp>
#include <net/transport.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace updclient::net {

// How a scheme treats Endpoint::port when the caller left it 0.
struct SchemeTraits {
  // Port filled in for this scheme regardless of the protocol; 0 for none.
  uint16_t defaultPort = 0;
  // The scheme carries the protocol's own IP port (49 for UpdServer, 80 for XeLL):
  // true for tcp, false for schemes that have no port or choose their own.
  bool usesProtocolPort = false;
};

// Thread-safe. instance() is the process-wide registry; independent instances
// can be created for isolation (e.g. in tests).
class UPDCLIENT_API TransportRegistry {
public:
  using Connector = std::function<Result<TransportPtr>(const Endpoint &)>;

  TransportRegistry() = default;
  TransportRegistry(const TransportRegistry &) = delete;
  TransportRegistry &operator=(const TransportRegistry &) = delete;

  static TransportRegistry &instance();

  // Schemes are case-insensitive. Registering an existing scheme replaces it.
  // The default traits leave port 0 untouched, which suits schemes without ports.
  void registerScheme(std::string scheme, Connector connector, SchemeTraits traits = {});
  bool unregisterScheme(std::string_view scheme);
  Result<TransportPtr> connect(const Endpoint &endpoint) const;
  // Protocol clients call this before connecting instead of writing a port into
  // the endpoint themselves. A non-zero port is kept. Otherwise the scheme's
  // defaultPort applies, then protocolPort if the scheme usesProtocolPort, and
  // port 0 is left alone for everything else. "tcp" counts as usesProtocolPort
  // even when it is not registered.
  Endpoint withDefaultPort(const Endpoint &endpoint, uint16_t protocolPort) const;
  std::vector<std::string> schemes() const;

private:
  mutable std::mutex mutex_;
  struct Entry {
    Connector connector;
    SchemeTraits traits;
  };
  std::map<std::string, Entry> entries_;
};

} // namespace updclient::net
