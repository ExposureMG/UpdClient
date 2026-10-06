#include "cli/args.hpp"
#include "cli/commands.hpp"
#include "cli/session.hpp"

#include <functional>

namespace updclient::cli {

namespace {

using PowerAction = std::function<Result<void>(updserver::UpdServerClient &)>;

void addPowerCommand(CLI::App *group, Context &context, const std::string &name, const std::string &description,
                     const std::string &action, PowerAction send) {
  auto *command = group->add_subcommand(name, description);
  command->callback([&context, name, action, send = std::move(send)] {
    context.finish(withUpdServer(
        context, action,
        [&](updserver::UpdServerClient &client, const net::Endpoint &endpoint) -> Outcome<void> {
          auto result = send(client);
          if (!result) return fromError(result.error());
          context.output.result({{"action", name}, {"target", endpoint.toString()}, {"acknowledged", false}},
                                "Sent " + name + " command to " + endpoint.toString() +
                                    " (the console does not acknowledge it)");
          return {};
        }));
  });
}

} // namespace

void registerPowerCommands(CLI::App &app, Context &context) {
  auto *power = addGroup(app, "power", "Console power management (destructive: needs --yes or confirmation)");
  addPowerCommand(power, context, "reboot", "Software reboot the console", "reboot the console",
                  [](updserver::UpdServerClient &client) { return client.reboot(); });
  addPowerCommand(power, context, "smc-reset", "Hardware SMC reset of the console", "SMC-reset the console",
                  [](updserver::UpdServerClient &client) { return client.smcReboot(); });
  addPowerCommand(power, context, "shutdown", "Shut the console down", "shut the console down",
                  [](updserver::UpdServerClient &client) { return client.shutdownConsole(); });
}

} // namespace updclient::cli
