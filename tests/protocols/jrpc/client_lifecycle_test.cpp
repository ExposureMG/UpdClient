#include "protocols/jrpc/client_fake.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/jrpc/client.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <tuple>
#include <vector>

// Connecting, the timeouts, the delivery record, close and reconnect, the trace hook
// and rawCommand, against the scripted fake of client_fake.hpp.

using namespace updclient;
using namespace updclient::jrpc;
using jt::FakeConsole;
using namespace std::chrono_literals;

namespace {

bool within(std::chrono::milliseconds value, std::chrono::milliseconds low, std::chrono::milliseconds high) {
  return value >= low && value <= high;
}

} // namespace

// --- Making a client ---------------------------------------------------------------

TEST(JrpcClientConnect, AttachWithoutATransportFails) {
  CHECK_ERR(JrpcClient::attach(nullptr), ErrorCode::ConnectFailed);
}

TEST(JrpcClientConnect, OpenUsesTheConnectorAndKeepsItForReconnect) {
  auto queue = std::make_shared<jt::ConsoleQueue>();
  auto first = FakeConsole::create();
  auto second = FakeConsole::create();
  queue->push_back(first);
  queue->push_back(second);
  auto client = JrpcClient::open(jt::connectorFor(queue), jt::quickOptions());
  REQUIRE_OK(client);
  CHECK(client->isConnected());
  CHECK_EQ(queue->size(), size_t{1});
  CHECK_OK(client->reconnect());
  CHECK_EQ(queue->size(), size_t{0});
  CHECK(first->closed());
  CHECK(!second->closed());
}

TEST(JrpcClientConnect, OpenPassesTheConnectorsErrorOn) {
  auto client = JrpcClient::open([]() -> Result<net::TransportPtr> { return fail(ErrorCode::ConnectFailed, "refused"); });
  REQUIRE_ERR(client, ErrorCode::ConnectFailed);
  CHECK_EQ(client.error().message, std::string("refused"));

  CHECK_ERR(JrpcClient::open(JrpcClient::Connector()), ErrorCode::InvalidArgument);
  CHECK_ERR(JrpcClient::open([]() -> Result<net::TransportPtr> { return net::TransportPtr(); }), ErrorCode::ConnectFailed);
}

TEST(JrpcClientConnect, AStoppedTokenSkipsTheConnector) {
  std::stop_source source;
  source.request_stop();
  int calls = 0;
  auto client = JrpcClient::open(
      [&]() -> Result<net::TransportPtr> {
        ++calls;
        return FakeConsole::create()->transport();
      },
      jt::quickOptions(), source.get_token());
  CHECK_ERR(client, ErrorCode::Cancelled);
  CHECK_EQ(calls, 0);
}

TEST(JrpcClientConnect, AStopRequestedWhileTheConnectorRunsClosesTheConnection) {
  std::stop_source source;
  auto console = FakeConsole::create();
  auto client = JrpcClient::open(
      [&]() -> Result<net::TransportPtr> {
        source.request_stop();
        return console->transport();
      },
      jt::quickOptions(), source.get_token());
  CHECK_ERR(client, ErrorCode::Cancelled);
  CHECK(console->closed());
}

TEST(JrpcClientConnect, ABadBannerFromTheConnectorEndsTheOpen) {
  auto console = FakeConsole::create(false);
  console->line("hello");
  auto client = JrpcClient::open([&]() -> Result<net::TransportPtr> { return console->transport(); }, jt::quickOptions());
  CHECK_ERR(client, ErrorCode::Protocol);
  CHECK(console->closed());
}

TEST(JrpcClientConnect, ConnectWithAnUnknownSchemeNeedsTheRegistry) {
  net::Endpoint endpoint;
  endpoint.scheme = "no-such-scheme";
  endpoint.host = "console";
  auto client = JrpcClient::connect(endpoint, jt::quickOptions());
  REQUIRE(!client.has_value());
  CHECK(client.error().code != ErrorCode::Unknown);
}

TEST(JrpcClientConnect, DescribeAndPeerWhenNotOverTcp) {
  auto console = FakeConsole::create();
  auto client = jt::connected(console);
  CHECK_EQ(client.describe(), std::string("fake-jrpc"));
  CHECK(!client.peer().has_value());
  client.close();
  CHECK(!client.peer().has_value());
}

TEST(JrpcClientConnect, AMovedFromClientRefusesEverything) {
  auto console = FakeConsole::create();
  auto client = jt::connected(console);
  JrpcClient moved = std::move(client);
  CHECK(moved.isConnected());
  CHECK(!client.isConnected());
  CHECK_ERR(client.call(jt::intCall()), ErrorCode::NotConnected);
  CHECK_ERR(client.rawCommand("x"), ErrorCode::NotConnected);
  CHECK_ERR(client.reconnect(), ErrorCode::NotConnected);
  CHECK(!client.lastDelivery().has_value());
  CHECK_EQ(client.describe(), std::string("<not connected>"));
  client.close();
  client.cancel();
  CHECK_EQ(console->byes(), 0);
}

TEST(JrpcClientConnect, MoveAssignmentSaysByeToTheOldConnection) {
  auto first = FakeConsole::create();
  auto second = FakeConsole::create();
  auto a = jt::connected(first);
  auto b = jt::connected(second);
  a = std::move(b);
  CHECK_EQ(first->byes(), 1);
  CHECK(first->closed());
  CHECK(a.isConnected());
  CHECK_EQ(second->byes(), 0);
}

// --- Timeouts ----------------------------------------------------------------------

TEST(JrpcClientTimeouts, EachWaitUsesItsOwnOption) {
  ClientOptions options = jt::quickOptions();
  options.bannerTimeout = 111ms;
  options.idleTimeout = 222ms;
  options.callTimeout = 3000ms;
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "2A\r\n");
  auto client = jt::connected(console, options);
  console->planReads({1});
  REQUIRE_OK(client.call(jt::intCall()));
  const auto &t = console->timeouts();
  // banner, the write, the wait for the first byte, then the gap before the next byte
  REQUIRE_EQ(t.size(), size_t{4});
  CHECK(within(t[0], 100ms, 111ms));
  CHECK_EQ(t[1], 222ms);
  CHECK(within(t[2], 2900ms, 3000ms));
  CHECK_EQ(t[3], 222ms);
}

TEST(JrpcClientTimeouts, ACallTimeoutOfZeroWaitsForTheFirstByteWithoutALimit) {
  ClientOptions options = jt::quickOptions();
  options.idleTimeout = 222ms;
  options.callTimeout = 0ms;
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "2A\r\n");
  auto client = jt::connected(console, options);
  console->planReads({1});
  REQUIRE_OK(client.call(jt::intCall()));
  const auto &t = console->timeouts();
  REQUIRE_EQ(t.size(), size_t{4});
  CHECK_EQ(t[1], 222ms);
  CHECK_EQ(t[2], 0ms);
  CHECK_EQ(t[3], 222ms);
}

TEST(JrpcClientTimeouts, ZeroDisablesTheBannerTimeoutToo) {
  ClientOptions options = jt::quickOptions();
  options.bannerTimeout = 0ms;
  auto console = FakeConsole::create();
  auto client = jt::connected(console, options);
  REQUIRE_EQ(console->timeouts().size(), size_t{1});
  CHECK_EQ(console->timeouts()[0], 0ms);
}

TEST(JrpcClientTimeouts, SilenceClosesTheConnection) {
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "");
  auto client = jt::connected(console);
  auto r = client.call(jt::intCall());
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(r.error().message.find("stopped responding") != std::string::npos);
  CHECK(!client.isConnected());
  CHECK(console->closed());
  // The function may still be running: the connection is not reused, and says no Bye.
  CHECK_ERR(client.call(jt::intCall()), ErrorCode::NotConnected);
  CHECK_EQ(console->commands().size(), size_t{1});
  client.close();
  CHECK_EQ(console->byes(), 0);
}

TEST(JrpcClientTimeouts, AStallInsideALineClosesTheConnection) {
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "2");
  auto client = jt::connected(console);
  auto r = client.call(jt::intCall());
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(r.error().message.find("nothing for 200 ms") != std::string::npos);
  CHECK(!client.isConnected());
}

TEST(JrpcClientTimeouts, ATimedOutCallSaysItMayHaveRun) {
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "");
  auto client = jt::connected(console);
  auto r = client.call(jt::intCall());
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(r.error().message.find("may have carried it out") != std::string::npos);
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK(delivery->delivery == Delivery::Sent);
}

// --- Delivery ----------------------------------------------------------------------

TEST(JrpcClientDelivery, TracksHowFarEachCommandGot) {
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "2A\r\n").on(std::string(jt::kIntCommand), "error=Version mismatch\r\n");
  auto client = jt::connected(console);
  CHECK(!client.lastDelivery().has_value());

  REQUIRE_OK(client.call(jt::intCall()));
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK_EQ(delivery->command, std::string("call"));
  CHECK(delivery->delivery == Delivery::Answered);

  CHECK_ERR(client.call(jt::intCall()), ErrorCode::Io);
  delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK(delivery->delivery == Delivery::Answered);
  // The next command is named after how it was sent.
  auto raw = client.rawCommand("x");
  CHECK(!raw.has_value());
  delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK_EQ(delivery->command, std::string("raw"));
}

TEST(JrpcClientDelivery, ARefusedCallWasNotSentAndSaysSo) {
  auto console = FakeConsole::create();
  auto client = jt::connected(console);
  std::vector<Arg> many(38, Arg::i32(1));
  auto r = client.call(jt::callAt(0x82000000, ReturnKind::Void, many));
  REQUIRE_ERR(r, ErrorCode::LimitExceeded);
  CHECK(r.error().message.find("may have carried it out") == std::string::npos);
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK_EQ(delivery->command, std::string("call"));
  CHECK(delivery->delivery == Delivery::NotSent);
  CHECK_EQ(console->written(), size_t{0});
  CHECK(client.isConnected());

  CHECK_ERR(client.call(jt::callAt(0x82000000, ReturnKind::Uint64Array, {}, 2)), ErrorCode::Unsupported);
  CHECK_ERR(client.call(jt::callAt(0, ReturnKind::Void)), ErrorCode::InvalidArgument);
  CHECK_ERR(client.call(jt::callAt(0x82000000, ReturnKind::IntArray, {}, 9)), ErrorCode::LimitExceeded);
  CHECK_ERR(client.call(jt::callAt(0x82000000, ReturnKind::Void, {Arg::f64(std::nan(""))})), ErrorCode::InvalidArgument);
  CHECK_EQ(console->written(), size_t{0});
  CHECK(client.isConnected());
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
}

TEST(JrpcClientDelivery, TheLineLimitIsTheOptions) {
  ClientOptions options = jt::quickOptions();
  options.maxCommandBytes = 60;
  auto console = FakeConsole::create();
  auto client = jt::connected(console, options);
  // "consolefeatures ver=2 type=0 as=0 params="A\82000000\A\0\"" is 58 bytes.
  console->on("consolefeatures ver=2 type=0 as=0 params=\"A\\82000000\\A\\0\\\"", "0\r\n");
  CHECK_OK(client.call(jt::callAt(0x82000000)));
  auto r = client.call(jt::callAt(0x82000000, ReturnKind::Void, {Arg::i32(1)}));
  CHECK_ERR(r, ErrorCode::LimitExceeded);
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
  CHECK(client.isConnected());

  ClientOptions arrays = jt::quickOptions();
  arrays.maxArrayElements = 9;
  auto other = FakeConsole::create();
  other->handle([](FakeConsole &c, const std::string &) { c.line("1,2,3,4,5,6,7,8,9;"); });
  auto wide = jt::connected(other, arrays);
  CHECK_OK(wide.call(jt::callAt(0x82000000, ReturnKind::IntArray, {}, 9)));
}

TEST(JrpcClientDelivery, AWriteThatDiesHalfWayIsPartlySent) {
  auto console = FakeConsole::create();
  console->dropAfterWritten(10);
  auto client = jt::connected(console);
  auto r = client.call(jt::intCall());
  REQUIRE_ERR(r, ErrorCode::Disconnected);
  CHECK(r.error().message.find("may have carried it out") != std::string::npos);
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK(delivery->delivery == Delivery::PartlySent);
  CHECK(!client.isConnected());
}

TEST(JrpcClientDelivery, ACallAfterCloseIsNotSent) {
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "2A\r\n");
  auto client = jt::connected(console);
  REQUIRE_OK(client.call(jt::intCall()));
  client.close();
  CHECK_ERR(client.call(jt::intCall()), ErrorCode::NotConnected);
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
}

TEST(JrpcClientDelivery, ReconnectKeepsTheRecordOfTheLastCall) {
  auto queue = std::make_shared<jt::ConsoleQueue>();
  auto first = FakeConsole::create();
  first->on(std::string(jt::kIntCommand), "");
  queue->push_back(FakeConsole::create());
  auto client = jt::connected(first, jt::quickOptions(), jt::connectorFor(queue));
  CHECK_ERR(client.call(jt::intCall()), ErrorCode::Timeout);
  REQUIRE_OK(client.reconnect());
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK(delivery->delivery == Delivery::Sent);
}

// --- close and reconnect -----------------------------------------------------------

TEST(JrpcClientClose, WritesOneByeAndNothingAfter) {
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "2A\r\n");
  auto client = jt::connected(console);
  REQUIRE_OK(client.call(jt::intCall()));
  const size_t before = console->written();
  client.close();
  CHECK(!client.isConnected());
  CHECK(console->closed());
  CHECK_EQ(console->byes(), 1);
  CHECK_EQ(console->wire(), std::string(jt::kIntCommand) + "\r\nBye\r\n");
  const size_t afterClose = console->written();
  CHECK_EQ(afterClose, before + 5);

  // Nothing goes out any more, whatever is asked.
  client.close();
  CHECK_ERR(client.call(jt::intCall()), ErrorCode::NotConnected);
  CHECK_ERR(client.rawCommand("anything"), ErrorCode::NotConnected);
  CHECK_ERR(client.call(jt::callAt(0x82000000, ReturnKind::Int, {Arg::i32(1)})), ErrorCode::NotConnected);
  CHECK_EQ(console->written(), afterClose);
  CHECK_EQ(console->byes(), 1);
  CHECK_EQ(console->problems(), std::string());
}

TEST(JrpcClientClose, TheDestructorSaysBye) {
  auto console = FakeConsole::create();
  {
    auto client = jt::connected(console);
    CHECK_EQ(console->byes(), 0);
  }
  CHECK_EQ(console->byes(), 1);
  CHECK(console->closed());
  CHECK_EQ(console->wire(), std::string("Bye\r\n"));
}

TEST(JrpcClientClose, NoByeAfterAFailureOrACancel) {
  {
    auto console = FakeConsole::create();
    console->handle([](FakeConsole &c, const std::string &) { c.line("nonsense"); });
    auto client = jt::connected(console);
    CHECK_ERR(client.call(jt::intCall()), ErrorCode::Protocol);
    client.close();
    CHECK_EQ(console->byes(), 0);
    CHECK_EQ(console->wire(), std::string(jt::kIntCommand) + "\r\n");
  }
  {
    auto console = FakeConsole::create();
    auto client = jt::connected(console);
    client.cancel();
    CHECK(!client.isConnected());
    client.close();
    CHECK_EQ(console->byes(), 0);
    CHECK_EQ(console->written(), size_t{0});
  }
}

TEST(JrpcClientClose, ByeWaitsForNoAnswer) {
  // The console never replies to Bye and close() must not wait for it.
  ClientOptions options = jt::quickOptions();
  options.byeTimeout = 50ms;
  auto console = FakeConsole::create();
  auto client = jt::connected(console, options);
  const auto start = std::chrono::steady_clock::now();
  client.close();
  CHECK(std::chrono::steady_clock::now() - start < 1s);
  CHECK_EQ(console->byes(), 1);
  CHECK(within(console->lastTimeout(), 40ms, 50ms));
}

TEST(JrpcClientClose, ByeIsBoundedByItsTimeoutWhenTheWriteDies) {
  auto console = FakeConsole::create();
  console->dropAfterWritten(0);
  auto client = jt::connected(console);
  client.close();
  CHECK(console->closed());
  CHECK_EQ(console->byes(), 0);
}

TEST(JrpcClientReconnect, OpensANewConnectionAndReadsItsBanner) {
  auto queue = std::make_shared<jt::ConsoleQueue>();
  auto first = FakeConsole::create();
  auto second = FakeConsole::create();
  first->on(std::string(jt::kIntCommand), "");
  second->on(std::string(jt::kIntCommand), "2A\r\n");
  queue->push_back(second);
  auto client = jt::connected(first, jt::quickOptions(), jt::connectorFor(queue));
  CHECK_ERR(client.call(jt::intCall()), ErrorCode::Timeout);
  CHECK(!client.isConnected());
  REQUIRE_OK(client.reconnect());
  CHECK(client.isConnected());
  auto r = client.call(jt::intCall());
  REQUIRE_OK(r);
  CHECK(r->value == CallValue(uint64_t{42}));
  CHECK_EQ(first->byes(), 0);
  CHECK_EQ(second->commands().size(), size_t{1});
}

TEST(JrpcClientReconnect, AHealthyConnectionGetsABye) {
  auto queue = std::make_shared<jt::ConsoleQueue>();
  auto first = FakeConsole::create();
  queue->push_back(FakeConsole::create());
  auto client = jt::connected(first, jt::quickOptions(), jt::connectorFor(queue));
  REQUIRE_OK(client.reconnect());
  CHECK_EQ(first->byes(), 1);
  CHECK(first->closed());
}

TEST(JrpcClientReconnect, WithoutAConnectorItIsUnsupportedAndChangesNothing) {
  auto console = FakeConsole::create();
  auto client = jt::connected(console);
  CHECK_ERR(client.reconnect(), ErrorCode::Unsupported);
  CHECK(client.isConnected());
  CHECK_EQ(console->written(), size_t{0});
}

TEST(JrpcClientReconnect, AFailedConnectLeavesTheClientDisconnected) {
  auto queue = std::make_shared<jt::ConsoleQueue>();
  auto first = FakeConsole::create();
  auto client = jt::connected(first, jt::quickOptions(), jt::connectorFor(queue));
  CHECK_ERR(client.reconnect(), ErrorCode::ConnectFailed);
  CHECK(!client.isConnected());
  CHECK_ERR(client.call(jt::intCall()), ErrorCode::NotConnected);

  auto bad = FakeConsole::create(false);
  bad->line("not jrpc");
  queue->push_back(bad);
  CHECK_ERR(client.reconnect(), ErrorCode::Protocol);
  CHECK(!client.isConnected());
  CHECK(bad->closed());

  queue->push_back(FakeConsole::create());
  CHECK_OK(client.reconnect());
  CHECK(client.isConnected());
}

TEST(JrpcClientReconnect, ClearsTheCancelledState) {
  auto queue = std::make_shared<jt::ConsoleQueue>();
  queue->push_back(FakeConsole::create());
  auto client = jt::connected(FakeConsole::create(), jt::quickOptions(), jt::connectorFor(queue));
  client.cancel();
  CHECK_ERR(client.call(jt::intCall()), ErrorCode::Cancelled);
  CHECK_OK(client.reconnect());
  CHECK(client.isConnected());
}

// --- Options and trace -------------------------------------------------------------

TEST(JrpcClientOptions, SetOptionsTakesEffectAtOnce) {
  auto console = FakeConsole::create();
  auto client = jt::connected(console);
  CHECK_EQ(client.options().idleTimeout, 200ms);
  ClientOptions changed = jt::quickOptions();
  changed.idleTimeout = 77ms;
  changed.callTimeout = 0ms;
  client.setOptions(changed);
  CHECK_EQ(client.options().idleTimeout, 77ms);
  console->on(std::string(jt::kIntCommand), "2A\r\n");
  REQUIRE_OK(client.call(jt::intCall()));
  CHECK_EQ(console->timeouts()[1], 77ms);
}

TEST(JrpcClientOptions, DefaultsAreThePlans) {
  const ClientOptions options;
  CHECK_EQ(options.bannerTimeout, 5000ms);
  CHECK_EQ(options.idleTimeout, 10000ms);
  CHECK_EQ(options.callTimeout, 60000ms);
  CHECK_EQ(options.byeTimeout, 500ms);
  CHECK_EQ(options.maxCommandBytes, size_t{8191});
  CHECK_EQ(options.maxReplyBytes, size_t{64 * 1024});
  CHECK_EQ(options.maxArrayElements, size_t{8});
  CHECK(options.silentOpBarrier);
  CHECK(!options.trace);
}

TEST(JrpcClientTrace, SeesTheBannerEveryLineAndTheBye) {
  struct Event {
    TraceEvent event;
    std::string text;
    uint64_t bytes;
  };
  std::vector<Event> events;
  ClientOptions options = jt::quickOptions();
  options.trace = [&](TraceEvent event, std::string_view text, uint64_t bytes) {
    events.push_back({event, std::string(text), bytes});
  };
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "2A\r\n");
  {
    auto client = jt::connected(console, options);
    REQUIRE_OK(client.call(jt::intCall()));
  }
  REQUIRE_EQ(events.size(), size_t{4});
  CHECK(events[0].event == TraceEvent::Received);
  CHECK_EQ(events[0].text, std::string("JRPC2 connected"));
  CHECK(events[1].event == TraceEvent::Sent);
  CHECK_EQ(events[1].text, std::string(jt::kIntCommand));
  CHECK(events[2].event == TraceEvent::Received);
  CHECK_EQ(events[2].text, std::string("2A"));
  CHECK(events[3].event == TraceEvent::Sent);
  CHECK_EQ(events[3].text, std::string("Bye"));
  for (const Event &e : events) CHECK_EQ(e.bytes, uint64_t{0});
}

TEST(JrpcClientTrace, AThrowingHookDoesNotBreakTheClient) {
  ClientOptions options = jt::quickOptions();
  options.trace = [](TraceEvent, std::string_view, uint64_t) { throw std::runtime_error("hook"); };
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "2A\r\n");
  auto client = jt::connected(console, options);
  CHECK_OK(client.call(jt::intCall()));
}

TEST(JrpcClientTrace, ACallFromTheHookIsRefusedAndTheOuterCallIsUntouched) {
  JrpcClient *self = nullptr;
  std::vector<Result<CallResult>> inner;
  ClientOptions options = jt::quickOptions();
  options.trace = [&](TraceEvent event, std::string_view, uint64_t) {
    if (event == TraceEvent::Sent && self) inner.push_back(self->call(jt::intCall()));
  };
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "2A\r\n");
  auto client = jt::connected(console, options);
  self = &client;
  auto r = client.call(jt::intCall());
  self = nullptr;
  REQUIRE_OK(r);
  CHECK(r->value == CallValue(uint64_t{42}));
  REQUIRE_EQ(inner.size(), size_t{1});
  CHECK_ERR(inner[0], ErrorCode::InvalidArgument);
  CHECK(inner[0].error().message.find("another call is in progress") != std::string::npos);
  CHECK_EQ(console->commands().size(), size_t{1});
  CHECK(client.isConnected());
  CHECK(client.lastDelivery()->delivery == Delivery::Answered);
}

// --- rawCommand --------------------------------------------------------------------

TEST(JrpcClientRaw, SendsTheLineAsTypedAndReturnsTheReplyLine) {
  auto console = FakeConsole::create();
  console->on("consolefeatures ver=2 type=13 params=\"A\\0\\A\\0\\\"", "17559\r\n").on("whatever you like", "reply text\r\n");
  auto client = jt::connected(console);
  auto kernel = client.rawCommand("consolefeatures ver=2 type=13 params=\"A\\0\\A\\0\\\"");
  REQUIRE_OK(kernel);
  CHECK_EQ(kernel->line, std::string("17559"));
  CHECK(!kernel->isError);
  auto other = client.rawCommand("whatever you like");
  REQUIRE_OK(other);
  CHECK_EQ(other->line, std::string("reply text"));
  CHECK_EQ(console->wire(), std::string("consolefeatures ver=2 type=13 params=\"A\\0\\A\\0\\\"\r\nwhatever you like\r\n"));
  CHECK_EQ(console->problems(), std::string());
  CHECK_EQ(client.lastDelivery()->command, std::string("raw"));
  CHECK(client.lastDelivery()->delivery == Delivery::Answered);
}

TEST(JrpcClientRaw, AnErrorLineIsAnAnswer) {
  auto console = FakeConsole::create();
  console->on("x", "error=Unknown command\r\n").on("y", "3\r\n");
  auto client = jt::connected(console);
  auto r = client.rawCommand("x");
  REQUIRE_OK(r);
  CHECK(r->isError);
  CHECK_EQ(r->line, std::string("error=Unknown command"));
  CHECK(client.isConnected());
  CHECK_OK(client.rawCommand("y"));
}

TEST(JrpcClientRaw, ADebugLineClosesTheConnection) {
  auto console = FakeConsole::create();
  console->on("x", "DEBUG\r\n");
  auto client = jt::connected(console);
  CHECK_ERR(client.rawCommand("x"), ErrorCode::Unsupported);
  CHECK(!client.isConnected());
}

TEST(JrpcClientRaw, ABadLineIsRefusedBeforeAnythingIsSent) {
  auto console = FakeConsole::create();
  auto client = jt::connected(console);
  CHECK_ERR(client.rawCommand(""), ErrorCode::InvalidArgument);
  CHECK_ERR(client.rawCommand("   "), ErrorCode::InvalidArgument);
  // A second command smuggled in behind an LF, or a CR, or a control character.
  CHECK_ERR(client.rawCommand("a\nb"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.rawCommand("a\r\nBye"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.rawCommand(std::string("a\0b", 3)), ErrorCode::InvalidArgument);
  CHECK_ERR(client.rawCommand("caf\xc3\xa9"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.rawCommand(std::string(8192, 'x')), ErrorCode::LimitExceeded);
  CHECK_EQ(console->written(), size_t{0});
  CHECK(client.isConnected());
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);

  console->on(std::string(8191, 'x'), "ok\r\n");
  CHECK_OK(client.rawCommand(std::string(8191, 'x')));
}

TEST(JrpcClientRaw, ACommandWithoutAnAnswerEndsInATimeout) {
  auto console = FakeConsole::create();
  console->on("consolefeatures ver=2 type=14 params=\"A\\0\\A\\0\\\"", "");
  auto client = jt::connected(console);
  auto r = client.rawCommand("consolefeatures ver=2 type=14 params=\"A\\0\\A\\0\\\"");
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(!client.isConnected());
  CHECK(client.lastDelivery()->delivery == Delivery::Sent);
}
