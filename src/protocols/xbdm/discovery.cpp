#include <protocols/xbdm/discovery.hpp>

#include <net/udp_socket.hpp>

#include "net/deadline.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>
#include <stop_token>
#include <utility>

namespace updclient::xbdm {

namespace {

using std::chrono::milliseconds;

constexpr milliseconds kPollSlice{200};

bool sameName(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
  });
}

discovery::DiscoveredDevice deviceFor(const std::string &address, const std::string &udpName, uint16_t port) {
  discovery::DiscoveredDevice device;
  device.protocol = "xbdm";
  device.address = address;
  device.info["name"] = udpName;
  device.info["udpName"] = udpName;
  device.info["port"] = std::to_string(port);
  device.lastSeen = std::chrono::system_clock::now();
  return device;
}

// Asks every console for its dbgname, within the budget, until a stop request.
// A stop during a query cancels it; consoles not asked keep their UDP names.
void resolveNamesWith(const DiscoveryOptions &discoveryOptions, const XbdmDiscovery::Connector &connector,
                      std::vector<discovery::DiscoveredDevice> &devices, std::stop_token stop) {
  if (!discoveryOptions.queryNames) return;
  const bool budgeted = discoveryOptions.nameQueryBudget.count() > 0;
  const auto budgetEnd = net::deadlineAfter(discoveryOptions.nameQueryBudget);
  size_t asked = 0;
  for (auto &device : devices) {
    if (stop.stop_requested()) {
      spdlog::debug("XBDM discovery: stopped before asking {} of {} consoles for their names", devices.size() - asked,
                    devices.size());
      break;
    }
    ClientOptions options = discoveryOptions.nameQuery;
    if (budgeted) {
      const auto remaining = std::chrono::floor<milliseconds>(budgetEnd - std::chrono::steady_clock::now());
      if (remaining <= milliseconds(0)) {
        spdlog::debug("XBDM discovery: no time left to ask {} of {} consoles for their names",
                      devices.size() - asked, devices.size());
        break;
      }
      // Every wait of the query fits in what is left of the budget.
      auto cap = [&](milliseconds &timeout) {
        if (timeout.count() == 0 || timeout > remaining) timeout = remaining;
      };
      cap(options.greetingTimeout);
      cap(options.idleTimeout);
      cap(options.commandTimeout);
      cap(options.byeTimeout);
    }
    ++asked;

    net::Endpoint endpoint;
    endpoint.scheme = "xbdm";
    endpoint.host = device.address;
    endpoint.port = discoveryOptions.port;
    endpoint.timeout = options.greetingTimeout;

    // An injected connector runs to its own end; the greeting is still cancelled.
    Result<XbdmClient> client =
        connector ? XbdmClient::open([connector, endpoint] { return connector(endpoint); }, options, stop)
                  : XbdmClient::connect(endpoint, options, stop);
    if (!client) {
      spdlog::debug("XBDM discovery: {} did not answer over TCP: {}", device.address, formatError(client.error()));
      continue;
    }
    Result<std::string> name = [&] {
      std::stop_callback onStop(stop, [&client] { client->cancel(); });
      return client->debugName();
    }();
    if (name && !name->empty()) {
      device.info["name"] = *name;
    } else if (!name) {
      spdlog::debug("XBDM discovery: dbgname on {} failed: {}", device.address, formatError(name.error()));
    }
  }
}

} // namespace

struct XbdmDiscovery::Search {
  std::vector<uint8_t> packet;
  std::string destination;
  // Empty accepts every name.
  std::string wantedName;
  // Empty accepts every sender.
  std::string wantedSender;
  bool stopAfterFirst = false;
  std::stop_token stop;
  // Set by run() when the stop request ended the search.
  mutable bool stopped = false;
};

XbdmDiscovery::XbdmDiscovery(net::DatagramSocketFactory socketFactory, DiscoveryOptions options, Connector connector)
    : socketFactory_(socketFactory ? std::move(socketFactory) : net::DatagramSocketFactory(net::makeUdpSocket)),
      options_(std::move(options)), connector_(std::move(connector)) {}

std::string XbdmDiscovery::name() const {
  return "xbdm";
}

Result<std::vector<discovery::DiscoveredDevice>> XbdmDiscovery::run(const Search &search, milliseconds timeout) {
  auto socket = socketFactory_();
  if (!socket) return fail(ErrorCode::Unknown, "datagram socket factory returned no socket");
  net::DatagramBindOptions bind;
  bind.broadcast = true;
  if (auto r = socket->bindWith(bind); !r) {
    Error error = r.error();
    error.message = "XBDM discovery cannot open a UDP socket: " + error.message;
    return unexpected<Error>(std::move(error));
  }

  const int sends = std::max(options_.sends, 1);
  const milliseconds wait = net::boundedWait(timeout);
  const milliseconds interval = std::max(wait / sends, milliseconds(1));
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + wait;
  auto nextSend = start;
  int sent = 0;

  std::vector<discovery::DiscoveredDevice> devices;
  std::set<std::string> seen;
  for (bool first = true;; first = false) {
    if (search.stop.stop_requested()) {
      search.stopped = true;
      spdlog::debug("XBDM discovery stopped after {} replies", devices.size());
      break;
    }
    auto now = std::chrono::steady_clock::now();
    if (sent < sends && now >= nextSend) {
      auto r = socket->sendTo(search.packet, search.destination, options_.port);
      if (!r) {
        if (sent == 0) {
          Error error = r.error();
          error.message = "XBDM discovery cannot send to " + search.destination + ": " + error.message;
          return unexpected<Error>(std::move(error));
        }
        spdlog::debug("XBDM discovery: resending to {} failed: {}", search.destination, formatError(r.error()));
      }
      ++sent;
      nextSend += interval;
    }
    if (!first && now >= deadline) break;

    auto until = deadline;
    if (sent < sends) until = std::min(until, nextSend);
    const auto remaining = std::chrono::ceil<milliseconds>(until - now);
    const auto slice = std::clamp(remaining, milliseconds(0), kPollSlice);

    auto packet = socket->receive(slice);
    if (!packet) {
      if (!devices.empty()) {
        spdlog::warn("XBDM discovery stopped early: {}", formatError(packet.error()));
        break;
      }
      return unexpected<Error>(packet.error());
    }
    if (!*packet) continue;

    const net::Datagram &datagram = **packet;
    auto replyName = parseNameReply(datagram.data);
    if (!replyName) {
      spdlog::debug("ignoring {} byte datagram from {} that is not an XBDM name reply", datagram.data.size(),
                    datagram.senderAddress);
      continue;
    }
    if (!search.wantedSender.empty() && datagram.senderAddress != search.wantedSender) continue;
    if (!search.wantedName.empty() && !sameName(*replyName, search.wantedName)) continue;
    if (seen.count(datagram.senderAddress) != 0) continue;
    if (devices.size() >= options_.maxDevices) {
      spdlog::warn("XBDM discovery: more than {} addresses answered; ignoring the rest", options_.maxDevices);
      break;
    }
    seen.insert(datagram.senderAddress);

    devices.push_back(deviceFor(datagram.senderAddress, *replyName, options_.port));
    spdlog::debug("XBDM console '{}' at {}", *replyName, datagram.senderAddress);
    if (search.stopAfterFirst) break;
  }
  socket->close();
  return devices;
}

void XbdmDiscovery::resolveNames(std::vector<discovery::DiscoveredDevice> &devices) {
  resolveNamesWith(options_, connector_, devices, std::stop_token());
}

Result<std::vector<discovery::DiscoveredDevice>> XbdmDiscovery::discover(milliseconds timeout, bool stopAfterFirst) {
  return discover(timeout, stopAfterFirst, std::stop_token());
}

Result<std::vector<discovery::DiscoveredDevice>> XbdmDiscovery::discover(milliseconds timeout, bool stopAfterFirst,
                                                                         std::stop_token stop) {
  Search search;
  search.packet = makeWildcardQuery();
  search.destination = options_.broadcastAddress;
  search.stopAfterFirst = stopAfterFirst;
  search.stop = stop;
  auto devices = run(search, timeout);
  if (!devices) return devices;
  if (!search.stopped) resolveNamesWith(options_, connector_, *devices, stop);
  return devices;
}

Result<std::optional<discovery::DiscoveredDevice>> XbdmDiscovery::findByName(std::string_view consoleName,
                                                                             milliseconds timeout) {
  return findByName(consoleName, timeout, std::stop_token());
}

Result<std::optional<discovery::DiscoveredDevice>> XbdmDiscovery::findByName(std::string_view consoleName,
                                                                             milliseconds timeout,
                                                                             std::stop_token stop) {
  auto packet = makeNameLookup(consoleName);
  if (!packet) return unexpected<Error>(packet.error());
  Search search;
  search.packet = std::move(*packet);
  search.destination = options_.broadcastAddress;
  search.wantedName = std::string(consoleName);
  search.stopAfterFirst = true;
  search.stop = stop;
  auto devices = run(search, timeout);
  if (!devices) return unexpected<Error>(devices.error());
  if (devices->empty()) return std::optional<discovery::DiscoveredDevice>{};
  if (!search.stopped) resolveNamesWith(options_, connector_, *devices, stop);
  return std::optional<discovery::DiscoveredDevice>(std::move(devices->front()));
}

Result<std::optional<discovery::DiscoveredDevice>> XbdmDiscovery::probeAddress(std::string_view address,
                                                                               milliseconds timeout) {
  return probeAddress(address, timeout, std::stop_token());
}

Result<std::optional<discovery::DiscoveredDevice>> XbdmDiscovery::probeAddress(std::string_view address,
                                                                               milliseconds timeout,
                                                                               std::stop_token stop) {
  if (address.empty()) return fail(ErrorCode::InvalidArgument, "no address to probe");
  Search search;
  search.packet = makeWildcardQuery();
  search.destination = std::string(address);
  search.wantedSender = std::string(address);
  search.stopAfterFirst = true;
  search.stop = stop;
  auto devices = run(search, timeout);
  if (!devices) return unexpected<Error>(devices.error());
  if (devices->empty()) return std::optional<discovery::DiscoveredDevice>{};
  if (!search.stopped) resolveNamesWith(options_, connector_, *devices, stop);
  return std::optional<discovery::DiscoveredDevice>(std::move(devices->front()));
}

Result<discovery::DiscoveredDevice> identify(const net::Endpoint &endpoint, ClientOptions options) {
  return identify(endpoint, std::move(options), std::stop_token());
}

Result<discovery::DiscoveredDevice> identify(const net::Endpoint &endpoint, ClientOptions options,
                                             std::stop_token stop) {
  auto client = XbdmClient::connect(endpoint, std::move(options), stop);
  if (!client) return unexpected<Error>(client.error());
  Result<std::string> name = [&] {
    std::stop_callback onStop(stop, [&client] { client->cancel(); });
    return client->debugName();
  }();
  if (!name) return unexpected<Error>(name.error());
  discovery::DiscoveredDevice device;
  device.protocol = "xbdm";
  device.address = endpoint.host;
  device.info["name"] = *name;
  device.info["port"] = std::to_string(endpoint.port != 0 ? endpoint.port : kXbdmPort);
  device.lastSeen = std::chrono::system_clock::now();
  return device;
}

void registerXbdmDiscovery(discovery::DiscoveryRegistry &registry) {
  registry.add(std::make_unique<XbdmDiscovery>());
}

void registerXbdm(net::TransportRegistry &transports, discovery::DiscoveryRegistry &providers) {
  registerXbdmScheme(transports);
  registerXbdmDiscovery(providers);
}

} // namespace updclient::xbdm
