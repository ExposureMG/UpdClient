#pragma once

#include "cli/context.hpp"

#include <updclient.hpp>

#include <functional>
#include <string>

namespace updclient::cli {

using UpdServerBody = std::function<Outcome<void>(updserver::UpdServerClient &, const net::Endpoint &)>;

// Resolves the target (explicit or discovered), connects and runs body. A non-empty
// destructiveAction ("erase 1 NAND block(s) starting at 0x10") makes the session
// ask for confirmation, after the target is known and before connecting.
Outcome<void> withUpdServer(Context &context, const std::string &destructiveAction, const UpdServerBody &body);

using XbdmBody = std::function<Outcome<void>(xbdm::XbdmClient &, const net::Endpoint &)>;

// Whether a command can change the console: only then does a failure say how far
// its command got (JSON "command_delivery").
enum class XbdmEffect { ReadOnly, ChangesConsole };

// As withUpdServer, for an XBDM console (Context::resolveXbdmEndpoint). From the
// connect to the end, Ctrl-C cancels what is in progress: the connect and greeting,
// the call, or the cleanup; an upload cut short has its temporary file deleted over
// a new connection, or named in a warning. A failure carries the client's kept
// upload and, for a command that changes the console, how far its command got.
// --trace records the session.
Outcome<void> withXbdm(Context &context, XbdmEffect effect, const std::string &destructiveAction,
                       const XbdmBody &body);

using JrpcBody = std::function<Outcome<void>(jrpc::JrpcClient &, const net::Endpoint &)>;

// Whether a command can change the console: only then does a failure say how far
// its command got (JSON "command_delivery"). Every call is treated as a change.
enum class JrpcEffect { ReadOnly, ChangesConsole };

// As withXbdm, for a JRPC console: --target or --ip is required (resolveJrpcEndpoint),
// since JRPC does not announce itself. --timeout-ms sets the banner, idle and call
// timeouts (0 for none); --trace records the session. Ctrl-C cancels the connect and
// banner, or the call in progress. A failed command that changes the console says how
// far its line got, unless the console answered it (an `error=` line is an answer).
Outcome<void> withJrpc(Context &context, JrpcEffect effect, const std::string &destructiveAction,
                       const JrpcBody &body);

// Is JRPC installed at the target? As withJrpc up to the banner, which is read and
// answered with Bye; no command is sent (jrpc::identify).
Outcome<jrpc::IdentifyResult> identifyJrpc(Context &context);

using XellBody = std::function<Outcome<void>(const xell::XellClient &, const net::Endpoint &)>;

// XeLL needs an explicit target; the client connects per request, so nothing is
// opened here.
Outcome<void> withXell(Context &context, const XellBody &body);

} // namespace updclient::cli
