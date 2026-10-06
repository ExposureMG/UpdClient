#include "cli/session.hpp"

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

Outcome<void> withXell(Context &context, const XellBody &body) {
  auto endpoint = context.resolveXellEndpoint();
  if (!endpoint) return unexpected<Failure>(endpoint.error());
  const xell::XellClient client = xell::XellClient::forEndpoint(*endpoint);
  return body(client, *endpoint);
}

} // namespace updclient::cli
