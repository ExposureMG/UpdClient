#pragma once

#include <core/error.hpp>
#include <core/expected.hpp>
#include <core/export.hpp>
#include <core/hex.hpp>
#include <core/path.hpp>
#include <discovery/discovery.hpp>
#include <net/datagram.hpp>
#include <net/endpoint.hpp>
#include <net/http_lite.hpp>
#include <net/tcp_transport.hpp>
#include <net/transport.hpp>
#include <net/transport_registry.hpp>
#include <net/udp_socket.hpp>
#include <protocols/updserver/client.hpp>
#include <protocols/updserver/discovery.hpp>
#include <protocols/updserver/protocol.hpp>
#include <protocols/xbdm/client.hpp>
#include <protocols/xbdm/discovery.hpp>
#include <protocols/xbdm/path.hpp>
#include <protocols/xbdm/protocol.hpp>
#include <protocols/xell/client.hpp>
#include <protocols/xell/discovery.hpp>

namespace updclient {

// Registers the "tcp" transport and the built-in discovery providers with the
// process-wide registries. Safe to call repeatedly and from several threads;
// only the first call has an effect. XBDM is not among them: call
// xbdm::registerXbdmScheme() or xbdm::registerXbdm() as well.
UPDCLIENT_API void registerBuiltins();

} // namespace updclient
