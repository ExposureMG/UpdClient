#pragma once

#include <updclient/core/error.hpp>
#include <updclient/core/expected.hpp>
#include <updclient/core/export.hpp>
#include <updclient/core/hex.hpp>
#include <updclient/core/path.hpp>
#include <updclient/discovery/discovery.hpp>
#include <updclient/net/datagram.hpp>
#include <updclient/net/endpoint.hpp>
#include <updclient/net/http_lite.hpp>
#include <updclient/net/tcp_transport.hpp>
#include <updclient/net/transport.hpp>
#include <updclient/net/transport_registry.hpp>
#include <updclient/net/udp_socket.hpp>
#include <updclient/protocols/updserver/client.hpp>
#include <updclient/protocols/updserver/discovery.hpp>
#include <updclient/protocols/updserver/protocol.hpp>
#include <updclient/protocols/xbdm/client.hpp>
#include <updclient/protocols/xbdm/discovery.hpp>
#include <updclient/protocols/xbdm/path.hpp>
#include <updclient/protocols/xbdm/protocol.hpp>
#include <updclient/protocols/xell/client.hpp>
#include <updclient/protocols/xell/discovery.hpp>

namespace updclient {

// Registers the "tcp" transport and the built-in discovery providers with the
// process-wide registries. Safe to call repeatedly and from several threads;
// only the first call has an effect. XBDM is not among them: call
// xbdm::registerXbdmScheme() or xbdm::registerXbdm() as well.
UPDCLIENT_API void registerBuiltins();

} // namespace updclient
