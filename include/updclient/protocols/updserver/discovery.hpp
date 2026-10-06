#pragma once

#include <updclient/core/export.hpp>
#include <updclient/discovery/discovery.hpp>
#include <updclient/net/datagram.hpp>
#include <updclient/protocols/updserver/protocol.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace updclient::updserver {

// Listens for the UpdServer UDP announcement broadcast (8 bytes: 'NSvr' magic and
// the console's IPv4 address, big-endian).
//
// Binding the announcement port (48 by default) is a privileged operation on Linux:
// run as root, grant CAP_NET_BIND_SERVICE, or lower net.ipv4.ip_unprivileged_port_start.
// A bind failure is returned as an Error, never as an empty successful result.
class UPDCLIENT_API UpdServerDiscovery final : public discovery::IDiscoveryProvider {
public:
  // An empty factory selects the real UDP socket.
  explicit UpdServerDiscovery(net::DatagramSocketFactory socketFactory = {},
                              uint16_t announcePort = ANNC_PORT);

  std::string name() const override;
  Result<std::vector<discovery::DiscoveredDevice>> discover(std::chrono::milliseconds timeout,
                                                            bool stopAfterFirst) override;

private:
  net::DatagramSocketFactory socketFactory_;
  uint16_t announcePort_;
};

// Adds an UpdServerDiscovery provider to the registry. Called by registerBuiltins().
UPDCLIENT_API void registerUpdServerDiscovery(
    discovery::DiscoveryRegistry &registry = discovery::DiscoveryRegistry::instance());

} // namespace updclient::updserver
