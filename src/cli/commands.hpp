#pragma once

#include "cli/context.hpp"

#include <CLI/CLI.hpp>

namespace updclient::cli {

// One function per command group. Each declares its subcommands on app and binds
// their callbacks to the shared context.
void registerDiscoverCommand(CLI::App &app, Context &context);
void registerInfoCommands(CLI::App &app, Context &context);
void registerPowerCommands(CLI::App &app, Context &context);
void registerNandCommands(CLI::App &app, Context &context);
void registerMemCommands(CLI::App &app, Context &context);
void registerFileCommands(CLI::App &app, Context &context);
void registerXellCommands(CLI::App &app, Context &context);
void registerXbdmCommands(CLI::App &app, Context &context);

} // namespace updclient::cli
