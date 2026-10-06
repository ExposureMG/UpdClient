#include "protocols/xbdm/client_fake.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <updclient/net/transport_registry.hpp>
#include <updclient/protocols/xbdm/discovery.hpp>

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace updclient;
using namespace updclient::xbdm;
using namespace std::chrono_literals;

namespace {

struct Sent {
  ut::Bytes data;
  std::string address;
  uint16_t port;
};

// A datagram network for the provider: what it sends is recorded, and replies are
// queued per send (the n-th send releases replies[n]), so a retry can be answered.
class FakeNetwork : public std::enable_shared_from_this<FakeNetwork> {
public:
  static std::shared_ptr<FakeNetwork> create() { return std::shared_ptr<FakeNetwork>(new FakeNetwork()); }

  FakeNetwork &replyToSend(size_t sendIndex, ut::Bytes data, std::string sender) {
    replies_[sendIndex].push_back(net::Datagram{std::move(data), std::move(sender), kXbdmPort});
    return *this;
  }
  FakeNetwork &failBind(Error error) {
    bindError_ = std::move(error);
    return *this;
  }
  FakeNetwork &failSend(Error error) {
    sendError_ = std::move(error);
    return *this;
  }

  const std::vector<Sent> &sent() const noexcept { return sent_; }
  bool boundForBroadcast() const noexcept { return broadcast_; }
  int sockets() const noexcept { return sockets_; }

  net::DatagramSocketFactory factory();

private:
  friend class FakeSocket;
  FakeNetwork() = default;

  std::map<size_t, std::vector<net::Datagram>> replies_;
  std::deque<net::Datagram> inbox_;
  std::vector<Sent> sent_;
  std::optional<Error> bindError_;
  std::optional<Error> sendError_;
  bool broadcast_ = false;
  int sockets_ = 0;
};

class FakeSocket final : public net::IDatagramSocket {
public:
  explicit FakeSocket(std::shared_ptr<FakeNetwork> network) : network_(std::move(network)) {}

  Result<void> bind(uint16_t, bool) override { return fail(ErrorCode::Unsupported, "use bindWith"); }

  Result<void> bindWith(const net::DatagramBindOptions &options) override {
    if (network_->bindError_) return unexpected<Error>(*network_->bindError_);
    network_->broadcast_ = options.broadcast;
    bound_ = true;
    return {};
  }

  Result<size_t> sendTo(std::span<const uint8_t> data, std::string_view address, uint16_t port) override {
    if (!bound_) return fail(ErrorCode::NotConnected, "not bound");
    if (network_->sendError_) return unexpected<Error>(*network_->sendError_);
    const size_t index = network_->sent_.size();
    network_->sent_.push_back(Sent{ut::Bytes(data.begin(), data.end()), std::string(address), port});
    auto it = network_->replies_.find(index);
    if (it != network_->replies_.end()) {
      for (auto &reply : it->second) network_->inbox_.push_back(reply);
    }
    return data.size();
  }

  Result<std::optional<net::Datagram>> receive(std::chrono::milliseconds timeout) override {
    if (!bound_) return fail(ErrorCode::NotConnected, "not bound");
    if (network_->inbox_.empty()) {
      std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(1)));
      return std::optional<net::Datagram>{};
    }
    auto datagram = std::move(network_->inbox_.front());
    network_->inbox_.pop_front();
    return std::optional<net::Datagram>(std::move(datagram));
  }

  void close() noexcept override { bound_ = false; }

private:
  std::shared_ptr<FakeNetwork> network_;
  bool bound_ = false;
};

net::DatagramSocketFactory FakeNetwork::factory() {
  auto self = shared_from_this();
  return [self]() -> std::unique_ptr<net::IDatagramSocket> {
    ++self->sockets_;
    return std::make_unique<FakeSocket>(self);
  };
}

ut::Bytes reply(std::string_view name) {
  ut::Bytes out{kNameReply, static_cast<uint8_t>(name.size())};
  ut::append(out, name);
  return out;
}

DiscoveryOptions udpOnly() {
  DiscoveryOptions options;
  options.queryNames = false;
  return options;
}

} // namespace

TEST(XbdmDiscovery, BroadcastsTheWildcardAndCollectsReplies) {
  auto network = FakeNetwork::create();
  network->replyToSend(0, reply("Kit A"), "192.168.1.10")
      .replyToSend(0, reply("Kit B"), "192.168.1.11")
      .replyToSend(0, ut::Bytes{1, 1, 'x'}, "192.168.1.12")
      .replyToSend(0, ut::Bytes{2, 9, 'x'}, "192.168.1.13")
      .replyToSend(1, reply("Kit A"), "192.168.1.10");
  XbdmDiscovery discovery(network->factory(), udpOnly());
  CHECK_EQ(discovery.name(), std::string("xbdm"));

  auto found = discovery.discover(150ms, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{2});
  CHECK_EQ((*found)[0].protocol, std::string("xbdm"));
  CHECK_EQ((*found)[0].address, std::string("192.168.1.10"));
  CHECK_EQ((*found)[0].info.at("name"), std::string("Kit A"));
  CHECK_EQ((*found)[0].info.at("udpName"), std::string("Kit A"));
  CHECK_EQ((*found)[0].info.at("port"), std::string("730"));
  CHECK_EQ((*found)[1].info.at("name"), std::string("Kit B"));

  REQUIRE_EQ(network->sent().size(), size_t{3});
  for (const auto &sent : network->sent()) {
    CHECK_EQ(sent.data, (ut::Bytes{3, 0}));
    CHECK_EQ(sent.address, std::string("255.255.255.255"));
    CHECK_EQ(sent.port, 730);
  }
  CHECK(network->boundForBroadcast());
}

TEST(XbdmDiscovery, StopsAtTheFirstReplyWhenAsked) {
  auto network = FakeNetwork::create();
  network->replyToSend(0, reply("One"), "10.0.0.1").replyToSend(0, reply("Two"), "10.0.0.2");
  XbdmDiscovery discovery(network->factory(), udpOnly());
  auto found = discovery.discover(5s, true);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ(network->sent().size(), size_t{1});
}

TEST(XbdmDiscovery, ARetryFindsALateConsole) {
  auto network = FakeNetwork::create();
  network->replyToSend(2, reply("Late"), "10.0.0.3");
  DiscoveryOptions options = udpOnly();
  options.broadcastAddress = "10.0.0.255";
  options.port = 7300;
  XbdmDiscovery discovery(network->factory(), options);
  auto found = discovery.discover(150ms, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{1});
  CHECK_EQ((*found)[0].address, std::string("10.0.0.3"));
  REQUIRE_EQ(network->sent().size(), size_t{3});
  CHECK_EQ(network->sent()[0].address, std::string("10.0.0.255"));
  CHECK_EQ(network->sent()[0].port, 7300);
}

TEST(XbdmDiscovery, NothingAnsweringIsAnEmptyResult) {
  auto network = FakeNetwork::create();
  XbdmDiscovery discovery(network->factory(), udpOnly());
  auto found = discovery.discover(30ms, false);
  REQUIRE_OK(found);
  CHECK(found->empty());
}

TEST(XbdmDiscovery, SocketFailuresAreErrors) {
  auto noBind = FakeNetwork::create();
  noBind->failBind(makeError(ErrorCode::Io, "no sockets here"));
  XbdmDiscovery first(noBind->factory(), udpOnly());
  CHECK_ERR(first.discover(30ms, false), ErrorCode::Io);

  auto noSend = FakeNetwork::create();
  noSend->failSend(makeError(ErrorCode::Io, "broadcast not permitted", 13));
  XbdmDiscovery second(noSend->factory(), udpOnly());
  auto r = second.discover(30ms, false);
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(r.error().message.find("255.255.255.255") != std::string::npos);
  CHECK_EQ(r.error().sysError, 13);

  XbdmDiscovery third([] { return std::unique_ptr<net::IDatagramSocket>(); }, udpOnly());
  CHECK_ERR(third.discover(30ms, false), ErrorCode::Unknown);
}

TEST(XbdmDiscovery, FindByNameMatchesCaseInsensitively) {
  auto network = FakeNetwork::create();
  network->replyToSend(0, reply("OTHER"), "10.0.0.8").replyToSend(0, reply("MyKit"), "10.0.0.9");
  XbdmDiscovery discovery(network->factory(), udpOnly());
  auto found = discovery.findByName("mykit", 60ms);
  REQUIRE_OK(found);
  REQUIRE(found->has_value());
  CHECK_EQ((*found)->address, std::string("10.0.0.9"));
  REQUIRE(!network->sent().empty());
  CHECK_EQ(network->sent()[0].data, (ut::Bytes{1, 5, 'm', 'y', 'k', 'i', 't'}));

  auto none = FakeNetwork::create();
  XbdmDiscovery quiet(none->factory(), udpOnly());
  auto missing = quiet.findByName("Nobody", 20ms);
  REQUIRE_OK(missing);
  CHECK(!missing->has_value());
  CHECK_ERR(quiet.findByName("", 20ms), ErrorCode::InvalidArgument);
}

TEST(XbdmDiscovery, ProbeAddressAcceptsOnlyThatSender) {
  auto network = FakeNetwork::create();
  network->replyToSend(0, reply("Elsewhere"), "10.0.0.1").replyToSend(0, reply("Here"), "10.0.0.5");
  XbdmDiscovery discovery(network->factory(), udpOnly());
  auto found = discovery.probeAddress("10.0.0.5", 60ms);
  REQUIRE_OK(found);
  REQUIRE(found->has_value());
  CHECK_EQ((*found)->info.at("name"), std::string("Here"));
  CHECK_EQ(network->sent()[0].address, std::string("10.0.0.5"));
  CHECK_EQ(network->sent()[0].data, (ut::Bytes{3, 0}));
  CHECK_ERR(discovery.probeAddress("", 10ms), ErrorCode::InvalidArgument);
}

TEST(XbdmDiscovery, DbgnameOverTcpReplacesTheUdpName) {
  auto network = FakeNetwork::create();
  network->replyToSend(0, reply("udp-a"), "10.0.0.1").replyToSend(0, reply("udp-b"), "10.0.0.2");
  std::vector<std::string> asked;
  auto connector = [&](const net::Endpoint &endpoint) -> Result<net::TransportPtr> {
    asked.push_back(endpoint.host + ":" + std::to_string(endpoint.port));
    if (endpoint.host == "10.0.0.2") return fail(ErrorCode::ConnectFailed, "refused");
    auto console = xt::FakeConsole::create();
    console->on("dbgname", "200- Real Name\r\n");
    return console->transport();
  };
  DiscoveryOptions options;
  options.queryNames = true;
  XbdmDiscovery discovery(network->factory(), options, connector);
  auto found = discovery.discover(30ms, false);
  REQUIRE_OK(found);
  REQUIRE_EQ(found->size(), size_t{2});
  CHECK_EQ((*found)[0].info.at("name"), std::string("Real Name"));
  CHECK_EQ((*found)[0].info.at("udpName"), std::string("udp-a"));
  CHECK_EQ((*found)[1].info.at("name"), std::string("udp-b"));
  CHECK_EQ(asked, (std::vector<std::string>{"10.0.0.1:730", "10.0.0.2:730"}));
}

TEST(XbdmDiscovery, IdentifyConnectsByAddressOnly) {
  auto &registry = net::TransportRegistry::instance();
  std::shared_ptr<xt::FakeConsole> console;
  registry.registerScheme("xbdmtestfake", [&](const net::Endpoint &endpoint) -> Result<net::TransportPtr> {
    if (endpoint.port != 730) return fail(ErrorCode::InvalidArgument, "expected port 730");
    console = xt::FakeConsole::create();
    console->on("dbgname", "200- Typed In\r\n");
    return console->transport();
  }, net::SchemeTraits{0, true});
  auto endpoint = net::Endpoint::parse("xbdmtestfake://10.1.2.3");
  REQUIRE_OK(endpoint);
  auto device = identify(*endpoint, xt::quickOptions());
  registry.unregisterScheme("xbdmtestfake");
  REQUIRE_OK(device);
  CHECK_EQ(device->protocol, std::string("xbdm"));
  CHECK_EQ(device->address, std::string("10.1.2.3"));
  CHECK_EQ(device->info.at("name"), std::string("Typed In"));
  CHECK_EQ(device->info.at("port"), std::string("730"));
  REQUIRE(console != nullptr);
  CHECK_EQ(console->byes(), 1);

  auto unknown = net::Endpoint::parse("nosuchscheme://10.1.2.3");
  REQUIRE_OK(unknown);
  CHECK_ERR(identify(*unknown), ErrorCode::Unsupported);
}

TEST(XbdmDiscovery, RegisterXbdmAddsTheSchemeAndTheProvider) {
  net::TransportRegistry transports;
  discovery::DiscoveryRegistry providers;
  registerXbdm(transports, providers);
  CHECK_EQ(transports.schemes(), std::vector<std::string>{"xbdm"});
  auto list = providers.providers();
  REQUIRE_EQ(list.size(), size_t{1});
  CHECK_EQ(list[0]->name(), std::string("xbdm"));
}
