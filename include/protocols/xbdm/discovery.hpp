#pragma once

#include <core/error.hpp>
#include <core/export.hpp>
#include <discovery/discovery.hpp>
#include <net/datagram.hpp>
#include <net/endpoint.hpp>
#include <net/transport.hpp>
#include <net/transport_registry.hpp>
#include <protocols/xbdm/client.hpp>
#include <protocols/xbdm/protocol.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace updclient::xbdm {

struct DiscoveryOptions {
  std::string broadcastAddress = "255.255.255.255";
  uint16_t port = kXbdmPort;
  // Datagrams sent per search, spread evenly over its timeout: the first and the
  // retries.
  int sends = 3;
  // Replies from more addresses than this are ignored: anyone on the network can
  // send them.
  size_t maxDevices = 256;
  // Ask every console that answered for its dbgname over TCP; that name replaces
  // the UDP one (section 2.2). A console that does not answer keeps its UDP name.
  bool queryNames = true;
  // For all dbgname queries of one search together, one after another; consoles
  // not asked by then keep their UDP names. Each query is also bounded by
  // nameQuery, and the TCP connect by its greetingTimeout. Zero is no budget.
  std::chrono::milliseconds nameQueryBudget{10000};
  ClientOptions nameQuery = [] {
    ClientOptions o;
    o.greetingTimeout = std::chrono::milliseconds(2000);
    o.idleTimeout = std::chrono::milliseconds(2000);
    o.commandTimeout = std::chrono::milliseconds(4000);
    o.byeTimeout = std::chrono::milliseconds(500);
    return o;
  }();
};

// Finds consoles with the XBDM name protocol on UDP port 730 (section 2): a
// type-3 wildcard broadcast that every console answers with its name. Devices are
// reported with protocol "xbdm", the reply's source address, and info "name",
// "udpName" (the name in the reply) and "port" (DiscoveryOptions::port, which is
// also the TCP port the dbgname query uses). Replies are de-duplicated by
// address. Whether every console answers UDP is not known, and networks often
// block broadcasts: identify() is the way in by address alone.
//
// One datagram socket, bound to an ephemeral port on the IPv4 wildcard address,
// sends the broadcast; the system picks the interface. The socket factory and the
// connector used for the dbgname query are injectable.
class UPDCLIENT_API XbdmDiscovery final : public discovery::IDiscoveryProvider {
public:
  using Connector = std::function<Result<net::TransportPtr>(const net::Endpoint &)>;

  // An empty factory selects the real UDP socket; an empty connector connects
  // over TCP.
  explicit XbdmDiscovery(net::DatagramSocketFactory socketFactory = {}, DiscoveryOptions options = {},
                         Connector connector = {});

  std::string name() const override;
  // Collects replies until the timeout, or until the first with stopAfterFirst.
  Result<std::vector<discovery::DiscoveredDevice>> discover(std::chrono::milliseconds timeout,
                                                            bool stopAfterFirst) override;
  // Type 1: the console of that name, compared case-insensitively.
  Result<std::optional<discovery::DiscoveredDevice>> findByName(std::string_view consoleName,
                                                                std::chrono::milliseconds timeout);
  // Type 3 sent to one address: is a console there?
  Result<std::optional<discovery::DiscoveredDevice>> probeAddress(std::string_view address,
                                                                  std::chrono::milliseconds timeout);

  // As above; a stop request ends the search, within one receive slice (200 ms)
  // while waiting for replies and at once during a dbgname query. A stopped search
  // succeeds with what it found so far: no name queries follow a stop in the UDP
  // phase, and consoles not asked keep their UDP names. With an injected connector
  // its TCP connect runs to its own end. The token is used only during the call.
  Result<std::vector<discovery::DiscoveredDevice>> discover(std::chrono::milliseconds timeout, bool stopAfterFirst,
                                                            std::stop_token stop);
  Result<std::optional<discovery::DiscoveredDevice>> findByName(std::string_view consoleName,
                                                                std::chrono::milliseconds timeout,
                                                                std::stop_token stop);
  Result<std::optional<discovery::DiscoveredDevice>> probeAddress(std::string_view address,
                                                                  std::chrono::milliseconds timeout,
                                                                  std::stop_token stop);

private:
  struct Search;
  Result<std::vector<discovery::DiscoveredDevice>> run(const Search &search, std::chrono::milliseconds timeout);
  void resolveNames(std::vector<discovery::DiscoveredDevice> &devices);

  net::DatagramSocketFactory socketFactory_;
  DiscoveryOptions options_;
  Connector connector_;
};

// Connect by address only, for networks where UDP is blocked or a console does
// not answer it: connects, reads the greeting and asks dbgname. The device has
// the endpoint's host as address and info "name" and "port".
UPDCLIENT_API Result<discovery::DiscoveredDevice> identify(const net::Endpoint &endpoint, ClientOptions options = {});
// As above; a stop request ends the connect, the greeting or the dbgname query at
// once with Cancelled.
UPDCLIENT_API Result<discovery::DiscoveredDevice> identify(const net::Endpoint &endpoint, ClientOptions options,
                                                           std::stop_token stop);

// Adds an XbdmDiscovery provider. Not part of registerBuiltins().
UPDCLIENT_API void registerXbdmDiscovery(
    discovery::DiscoveryRegistry &registry = discovery::DiscoveryRegistry::instance());

// registerXbdmScheme and registerXbdmDiscovery together.
UPDCLIENT_API void registerXbdm(net::TransportRegistry &transports = net::TransportRegistry::instance(),
                                discovery::DiscoveryRegistry &providers = discovery::DiscoveryRegistry::instance());

} // namespace updclient::xbdm
