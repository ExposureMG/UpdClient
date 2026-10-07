#include "cli/session.hpp"

#include "cli/interrupt.hpp"
#include "cli/trace.hpp"

#include <spdlog/spdlog.h>

#include <memory>

namespace updclient::cli {

Outcome<void> withUpdServer(Context &context, const std::string &destructiveAction, const UpdServerBody &body) {
  const bool destructive = !destructiveAction.empty();
  if (destructive) {
    if (auto possible = context.requireConfirmationPossible(); !possible) return possible;
  }

  auto endpoint = context.resolveUpdServerEndpoint();
  if (!endpoint) return unexpected<Failure>(endpoint.error());

  if (destructive) {
    if (auto confirmed = context.confirmDestructive(destructiveAction, endpoint->toString()); !confirmed) {
      return confirmed;
    }
  }

  auto client = updserver::UpdServerClient::connect(*endpoint);
  if (!client) return fromError(client.error());
  return body(*client, *endpoint);
}

Outcome<void> withXbdm(Context &context, const std::string &destructiveAction, const XbdmBody &body) {
  const bool destructive = !destructiveAction.empty();
  if (destructive) {
    if (auto possible = context.requireConfirmationPossible(); !possible) return possible;
  }

  auto endpoint = context.resolveXbdmEndpoint();
  if (!endpoint) return unexpected<Failure>(endpoint.error());

  if (destructive) {
    if (auto confirmed = context.confirmDestructive(destructiveAction, endpoint->toString()); !confirmed) {
      return confirmed;
    }
  }

  xbdm::ClientOptions options;
  if (context.options.timeoutMs) {
    const std::chrono::milliseconds timeout(*context.options.timeoutMs);
    options.greetingTimeout = timeout;
    options.idleTimeout = timeout;
  }
  std::unique_ptr<TraceFile> trace;
  if (!context.options.trace.empty()) {
    auto opened = TraceFile::open(context.options.trace, endpoint->toString());
    if (!opened) return unexpected<Failure>(opened.error());
    trace = std::move(*opened);
    options.trace = trace->hook();
  }

  auto client = xbdm::XbdmClient::connect(*endpoint, options);
  if (!client) return fromError(client.error());

  Outcome<void> outcome;
  {
    InterruptScope interrupt([&client] { client->cancel(); });
    outcome = body(*client, *endpoint);
  }

  const auto leftovers = client->pendingCleanup();
  if (!leftovers.empty() && !client->transferActive()) {
    if (auto cleaned = client->reconnect(); cleaned && client->pendingCleanup().empty()) {
      for (const auto &name : leftovers) spdlog::info("Deleted the unfinished upload {}", name);
    } else {
      for (const auto &name : client->pendingCleanup()) {
        spdlog::warn("The unfinished upload {} is still on the console; delete it with 'xbdm rm'", name);
      }
    }
  }
  return outcome;
}

Outcome<void> withXell(Context &context, const XellBody &body) {
  auto endpoint = context.resolveXellEndpoint();
  if (!endpoint) return unexpected<Failure>(endpoint.error());
  const xell::XellClient client = xell::XellClient::forEndpoint(*endpoint);
  return body(client, *endpoint);
}

} // namespace updclient::cli
