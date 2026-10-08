#pragma once

#include "cli/context.hpp"

namespace updclient::cli {

// The JRPC side of the commands that several protocols share. info and power
// shutdown call these when --target is a jrpc:// endpoint. The mem, file and nand
// commands have no JRPC form: JRPC has no memory or file commands, and the UpdServer
// resolver refuses the target.
Outcome<void> jrpcInfo(Context &context);
Outcome<void> jrpcShutdown(Context &context);

} // namespace updclient::cli
