#pragma once

#include "cli/context.hpp"

#include <updclient/updclient.hpp>

#include <functional>
#include <string>

namespace updclient::cli {

using UpdServerBody = std::function<Outcome<void>(updserver::UpdServerClient &, const net::Endpoint &)>;

// Resolves the target (explicit or discovered), connects and runs body. A non-empty
// destructiveAction ("erase 1 NAND block(s) starting at 0x10") makes the session
// ask for confirmation, after the target is known and before connecting.
Outcome<void> withUpdServer(Context &context, const std::string &destructiveAction, const UpdServerBody &body);

using XbdmBody = std::function<Outcome<void>(xbdm::XbdmClient &, const net::Endpoint &)>;

// As withUpdServer, for an XBDM console (Context::resolveXbdmEndpoint). While body
// runs, Ctrl-C cancels the call in progress; an upload cut short that way has its
// temporary file deleted over a new connection, or named in a warning. --trace
// records the session.
Outcome<void> withXbdm(Context &context, const std::string &destructiveAction, const XbdmBody &body);

using XellBody = std::function<Outcome<void>(const xell::XellClient &, const net::Endpoint &)>;

// XeLL needs an explicit target; the client connects per request, so nothing is
// opened here.
Outcome<void> withXell(Context &context, const XellBody &body);

} // namespace updclient::cli
