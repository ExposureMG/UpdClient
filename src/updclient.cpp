#include <updclient.hpp>

#include <memory>
#include <mutex>

namespace updclient {

void registerBuiltins() {
  static std::once_flag once;
  std::call_once(once, [] {
    net::SchemeTraits tcpTraits;
    tcpTraits.usesProtocolPort = true;
    net::TransportRegistry::instance().registerScheme(
        "tcp", [](const net::Endpoint &endpoint) { return net::TcpTransport::connect(endpoint); }, tcpTraits);
    discovery::DiscoveryRegistry::instance().add(std::make_unique<updserver::UpdServerDiscovery>());
  });
}

} // namespace updclient
