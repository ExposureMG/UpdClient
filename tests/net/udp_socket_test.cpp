#include "support/fake_datagram_socket.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <net/udp_socket.hpp>

#include <chrono>
#include <thread>

using namespace updclient;
using namespace std::chrono_literals;
using ut::Bytes;

namespace {

net::DatagramBindOptions loopbackOptions() {
  net::DatagramBindOptions options;
  options.address = "127.0.0.1";
  return options;
}

void bindOrSkip(net::UdpSocket &socket, const net::DatagramBindOptions &options) {
  if (auto r = socket.bindWith(options); !r) SKIP("udp sockets are not available here: " + formatError(r.error()));
}

} // namespace

TEST(UdpSocket, SendToDeliversToAnotherSocket) {
  net::UdpSocket receiver;
  bindOrSkip(receiver, loopbackOptions());
  net::UdpSocket sender;
  bindOrSkip(sender, loopbackOptions());

  const Bytes payload = ut::patternBytes(40, 3);
  auto sent = sender.sendTo(payload, "127.0.0.1", receiver.boundPort());
  REQUIRE_OK(sent);
  CHECK_EQ(*sent, payload.size());

  auto got = receiver.receive(2000ms);
  REQUIRE_OK(got);
  REQUIRE(got->has_value());
  CHECK_EQ((*got)->data, payload);
  CHECK_EQ((*got)->senderAddress, std::string("127.0.0.1"));
  CHECK_EQ((*got)->senderPort, sender.boundPort());
}

TEST(UdpSocket, BindWithPortZeroReportsTheChosenPort) {
  net::UdpSocket socket;
  bindOrSkip(socket, loopbackOptions());
  CHECK(socket.isBound());
  CHECK(socket.boundPort() != 0);
  socket.close();
  CHECK(!socket.isBound());
}

TEST(UdpSocket, BroadcastOptionIsAccepted) {
  net::UdpSocket socket;
  auto options = loopbackOptions();
  options.broadcast = true;
  bindOrSkip(socket, options);
  CHECK(socket.isBound());
}

TEST(UdpSocket, SendToNeedsABoundSocketAndAPort) {
  net::UdpSocket unbound;
  const Bytes payload{1, 2, 3};
  CHECK_ERR(unbound.sendTo(payload, "127.0.0.1", 9), ErrorCode::NotConnected);

  net::UdpSocket socket;
  bindOrSkip(socket, loopbackOptions());
  CHECK_ERR(socket.sendTo(payload, "127.0.0.1", 0), ErrorCode::InvalidArgument);
  CHECK_ERR(socket.sendTo(payload, "not-an-address", 9), ErrorCode::InvalidArgument);
  // Datagram addresses stay numeric even though TCP endpoints accept host names.
  CHECK_ERR(socket.sendTo(payload, "localhost", 9), ErrorCode::InvalidArgument);
}

TEST(UdpSocket, BadBindOptionsAreInvalidArguments) {
  net::UdpSocket socket;
  net::DatagramBindOptions options;
  options.address = "not-an-address";
  CHECK_ERR(socket.bindWith(options), ErrorCode::InvalidArgument);

  options = {};
  options.multicastGroup = "not-a-group";
  CHECK_ERR(socket.bindWith(options), ErrorCode::InvalidArgument);
  CHECK(!socket.isBound());
}

TEST(UdpSocket, ReceiveWithAnEffectivelyInfiniteTimeoutWaitsForTheDatagram) {
  net::UdpSocket receiver;
  bindOrSkip(receiver, loopbackOptions());
  const uint16_t port = receiver.boundPort();

  std::thread sender([port] {
    std::this_thread::sleep_for(150ms);
    net::UdpSocket socket;
    if (!socket.bindWith(loopbackOptions())) return;
    const Bytes payload{9, 9};
    (void)socket.sendTo(payload, "127.0.0.1", port);
  });
  auto got = receiver.receive(std::chrono::milliseconds::max());
  sender.join();

  REQUIRE_OK(got);
  REQUIRE(got->has_value());
  CHECK_EQ((*got)->data, (Bytes{9, 9}));
}

TEST(UdpSocket, ReceiveRequiresBinding) {
  net::UdpSocket socket;
  CHECK_ERR(socket.receive(1ms), ErrorCode::NotConnected);
}

TEST(DatagramSocketDefaults, BindWithForwardsPlainListenerOptions) {
  auto fake = ut::FakeDatagrams::create();
  auto socket = fake->factory()();
  net::DatagramBindOptions options;
  options.port = 4848;
  options.reuse = true;
  REQUIRE_OK(socket->bindWith(options));
  CHECK_EQ(fake->bindCalls(), 1);
  CHECK_EQ(fake->boundPort(), 4848);
  CHECK(fake->boundWithReuse());
}

TEST(DatagramSocketDefaults, BindWithRejectsWhatABasicSocketCannotDo) {
  auto fake = ut::FakeDatagrams::create();
  auto socket = fake->factory()();

  net::DatagramBindOptions options;
  options.broadcast = true;
  CHECK_ERR(socket->bindWith(options), ErrorCode::Unsupported);
  options = {};
  options.multicastGroup = "239.255.255.250";
  CHECK_ERR(socket->bindWith(options), ErrorCode::Unsupported);
  options = {};
  options.family = net::AddressFamily::IPv6;
  CHECK_ERR(socket->bindWith(options), ErrorCode::Unsupported);
  options = {};
  options.address = "127.0.0.1";
  CHECK_ERR(socket->bindWith(options), ErrorCode::Unsupported);
  CHECK_EQ(fake->bindCalls(), 0);

  const Bytes payload{1};
  CHECK_ERR(socket->sendTo(payload, "127.0.0.1", 9), ErrorCode::Unsupported);
}
