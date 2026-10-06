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

using XellBody = std::function<Outcome<void>(const xell::XellClient &, const net::Endpoint &)>;

// XeLL needs an explicit target; the client connects per request, so nothing is
// opened here.
Outcome<void> withXell(Context &context, const XellBody &body);

} // namespace updclient::cli
