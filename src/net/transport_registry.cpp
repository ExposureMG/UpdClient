#include <updclient/net/transport_registry.hpp>

#include <algorithm>
#include <cctype>

namespace updclient::net {

namespace {

std::string normalize(std::string_view scheme) {
  std::string out(scheme);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

} // namespace

TransportRegistry &TransportRegistry::instance() {
  static TransportRegistry registry;
  return registry;
}

void TransportRegistry::registerScheme(std::string scheme, Connector connector, SchemeTraits traits) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_[normalize(scheme)] = Entry{std::move(connector), traits};
}

bool TransportRegistry::unregisterScheme(std::string_view scheme) {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.erase(normalize(scheme)) > 0;
}

Result<TransportPtr> TransportRegistry::connect(const Endpoint &endpoint) const {
  Connector connector;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(normalize(endpoint.scheme));
    if (it != entries_.end()) connector = it->second.connector;
  }
  if (!connector) {
    return fail(ErrorCode::Unsupported,
                "no transport registered for scheme '" + endpoint.scheme +
                    "' (has updclient::registerBuiltins() been called?)");
  }
  return connector(endpoint);
}

Endpoint TransportRegistry::withDefaultPort(const Endpoint &endpoint, uint16_t protocolPort) const {
  Endpoint result = endpoint;
  if (result.port != 0) return result;

  const std::string scheme = normalize(endpoint.scheme);
  SchemeTraits traits;
  traits.usesProtocolPort = scheme == "tcp";
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(scheme);
    if (it != entries_.end()) traits = it->second.traits;
  }
  if (traits.defaultPort != 0) {
    result.port = traits.defaultPort;
  } else if (traits.usesProtocolPort) {
    result.port = protocolPort;
  }
  return result;
}

std::vector<std::string> TransportRegistry::schemes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (const auto &entry : entries_) out.push_back(entry.first);
  return out;
}

} // namespace updclient::net
