#include "protocols/jrpc/client_fake.hpp"
#include "support/jrpc_mock_server.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/jrpc/client.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

// A short check of the client against the mock server, whose parser was written from the
// protocol document and shares no code with the client: whatever the client builds has to
// be understood by it, and whatever the mock replies has to be read by the client. The
// full set (every kind, every opcode, loopback TCP) is the integration tests' job.

using namespace updclient;
using namespace updclient::jrpc;
using namespace std::chrono_literals;
using ut::JrpcCall;
using ut::JrpcFault;
using ut::JrpcMockOptions;
using ut::JrpcMockServer;
using ut::JrpcReturn;

namespace {

ClientOptions options() {
  ClientOptions o = jt::quickOptions();
  o.callTimeout = 3000ms;
  o.idleTimeout = 1000ms;
  return o;
}

Result<JrpcClient> connectTo(JrpcMockServer &mock, ClientOptions o = options()) {
  return JrpcClient::open(mock.connector(), std::move(o));
}

} // namespace

TEST(JrpcClientMock, CallsReachTheRegisteredFunctionWithTheirArguments) {
  JrpcMockServer mock;
  mock.registerFunction(0x82010000u, [](const JrpcCall &call) {
    return JrpcReturn::integer(static_cast<uint64_t>(call.args.at(0).integer + call.args.at(1).integer));
  });
  mock.registerFunction(0x82020000u, [](const JrpcCall &call) { return JrpcReturn::string(call.args.at(0).text() + "!"); });
  mock.registerFunction("xam.xex", 436, [](const JrpcCall &call) {
    CHECK(call.system);
    return JrpcReturn::integer(0x248173A00ull);
  });
  auto client = connectTo(mock);
  REQUIRE_OK(client);

  auto sum = client->call(jt::intCall());
  REQUIRE_OK(sum);
  CHECK(sum->value == CallValue(uint64_t{21}));
  CHECK_EQ(sum->line, std::string("15"));

  auto text = client->call(jt::callAt(0x82020000, ReturnKind::String, {Arg::string("hi")}));
  REQUIRE_OK(text);
  CHECK(text->value == CallValue(std::string("hi!")));

  CallSpec byOrdinal;
  byOrdinal.target = ByName{"xam.xex", 436};
  byOrdinal.thread = ThreadContext::System;
  byOrdinal.returns = ReturnKind::Int64;
  auto big = client->call(byOrdinal);
  REQUIRE_OK(big);
  CHECK(big->value == CallValue(uint64_t{0x248173A00}));

  CHECK(client->isConnected());
  CHECK_EQ(mock.calls().size(), size_t{3});
}

TEST(JrpcClientMock, ArrayAndFloatReturnsAreRead) {
  JrpcMockServer mock;
  mock.registerFunction(0x82000000u, [](const JrpcCall &call) {
    switch (call.type) {
    case 3: return JrpcReturn::real(2.5);
    case 5: return JrpcReturn::intArray({1, -2, 3});
    case 6: return JrpcReturn::floatArray({1.5, 2.0});
    default: return JrpcReturn::byteArray({1, 255, 10});
    }
  });
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  auto f = client->call(jt::callAt(0x82000000, ReturnKind::Float));
  REQUIRE_OK(f);
  CHECK(f->value == CallValue(2.5));
  auto ints = client->call(jt::callAt(0x82000000, ReturnKind::IntArray, {}, 3));
  REQUIRE_OK(ints);
  CHECK(ints->value == CallValue(std::vector<int32_t>{1, -2, 3}));
  auto floats = client->call(jt::callAt(0x82000000, ReturnKind::FloatArray, {}, 2));
  REQUIRE_OK(floats);
  CHECK(floats->value == CallValue(std::vector<double>{1.5, 2.0}));
  auto bytes = client->call(jt::callAt(0x82000000, ReturnKind::ByteArray, {}, 3));
  REQUIRE_OK(bytes);
  CHECK(bytes->value == CallValue(std::vector<uint8_t>{1, 255, 10}));
}

TEST(JrpcClientMock, CloseSendsByeAndTheMockRecordsIt) {
  JrpcMockServer mock;
  mock.registerFunction(0x82010000u, [](const JrpcCall &) { return JrpcReturn::integer(1); });
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  REQUIRE_OK(client->call(jt::intCall()));
  client->close();
  REQUIRE(mock.waitForCommands(2));
  const auto commands = mock.commands();
  REQUIRE_EQ(commands.size(), size_t{2});
  CHECK_EQ(commands[0].line, std::string(jt::kIntCommand));
  CHECK_EQ(commands[1].name, std::string("bye"));
  CHECK(mock.waitForActiveConnections(0));
}

TEST(JrpcClientMock, AnUnknownFunctionIsARemoteFaultAndTheConnectionStays) {
  JrpcMockServer mock;
  mock.registerFunction(0x82010000u, [](const JrpcCall &) { return JrpcReturn::integer(1); });
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  auto r = client->call(jt::callAt(0x83000000, ReturnKind::Int));
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(remoteFault(r.error()) == std::optional<RemoteFault>(RemoteFault::CouldNotResolve));
  CHECK(client->isConnected());
  CHECK_OK(client->call(jt::intCall()));

  auto byName = client->call([] {
    CallSpec spec;
    spec.target = ByName{"nothing.xex", 1};
    return spec;
  }());
  REQUIRE_ERR(byName, ErrorCode::Io);
  CHECK(remoteFault(byName.error()).has_value());
  CHECK(client->isConnected());
}

TEST(JrpcClientMock, AFunctionThatAnswersErrorIsARemoteFault) {
  JrpcMockServer mock;
  mock.registerFunction(0x82010000u, [](const JrpcCall &) { return JrpcReturn::failure("Version mismatch"); });
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  auto r = client->call(jt::intCall());
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(remoteFault(r.error()) == std::optional<RemoteFault>(RemoteFault::VersionMismatch));
  CHECK(client->isConnected());
}

TEST(JrpcClientMock, RawCommandExchangesOneLine) {
  JrpcMockServer mock;
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  auto kernel = client->rawCommand(R"JR(consolefeatures ver=2 type=13 params="A\0\A\0\")JR");
  REQUIRE_OK(kernel);
  CHECK_EQ(kernel->line, std::string("17559"));
  auto bad = client->rawCommand("not a command");
  REQUIRE_OK(bad);
  CHECK(bad->isError);
  CHECK(client->isConnected());
}

TEST(JrpcClientMock, FaultsOnTheReplyCloseTheConnectionAsPromised) {
  struct Row {
    const char *name;
    JrpcFault fault;
    ErrorCode code;
  };
  const std::vector<Row> rows = {
      {"dropped after one byte", JrpcFault::dropAfterBytes(1).onType(1), ErrorCode::Disconnected},
      {"dropped at once", JrpcFault::dropConnection().onType(1), ErrorCode::Disconnected},
      {"debug line", JrpcFault::debugLine().onType(1), ErrorCode::Unsupported},
      {"wrong shape", JrpcFault::replyLine("hello").onType(1), ErrorCode::Protocol},
      {"over-long line", JrpcFault::oversizedLine().onType(1), ErrorCode::LimitExceeded},
      {"silence", JrpcFault::silence().onType(1), ErrorCode::Timeout},
  };
  for (const Row &row : rows) {
    JrpcMockServer mock;
    mock.registerFunction(0x82010000u, [](const JrpcCall &) { return JrpcReturn::integer(1); });
    mock.inject(row.fault);
    ClientOptions o = options();
    o.callTimeout = 200ms;
    o.idleTimeout = 200ms;
    auto client = connectTo(mock, o);
    REQUIRE_OK(client);
    auto r = client->call(jt::intCall());
    CHECK_MSG(!r.has_value() && r.error().code == row.code, row.name);
    CHECK_MSG(!client->isConnected(), row.name);
    CHECK_ERR(client->call(jt::intCall()), ErrorCode::NotConnected);
  }
}

TEST(JrpcClientMock, AnErrorLineFromTheMockKeepsTheConnection) {
  JrpcMockServer mock;
  mock.registerFunction(0x82010000u, [](const JrpcCall &) { return JrpcReturn::integer(1); });
  mock.inject(JrpcFault::errorLine("The paramaters were not found").onType(1).withoutCommand());
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  auto r = client->call(jt::intCall());
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(remoteFault(r.error()) == std::optional<RemoteFault>(RemoteFault::ParametersNotFound));
  CHECK_OK(client->call(jt::intCall()));
}

TEST(JrpcClientMock, TrailingBytesBreakTheNextCall) {
  JrpcMockServer mock;
  mock.registerFunction(0x82010000u, [](const JrpcCall &) { return JrpcReturn::integer(1); });
  mock.inject(JrpcFault::extraBytes(ut::bytesOf("2B\r\n")).onType(1));
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  CHECK_OK(client->call(jt::intCall()));
  CHECK_ERR(client->call(jt::intCall()), ErrorCode::Protocol);
  CHECK(!client->isConnected());
}

TEST(JrpcClientMock, ABannerThatIsMissingOrWrongEndsTheOpen) {
  {
    JrpcMockOptions o;
    o.sendBanner = false;
    JrpcMockServer mock(o);
    ClientOptions c = options();
    c.bannerTimeout = 100ms;
    CHECK_ERR(connectTo(mock, c), ErrorCode::Timeout);
  }
  {
    JrpcMockOptions o;
    o.banner = "JRPC connected";
    JrpcMockServer mock(o);
    CHECK_ERR(connectTo(mock), ErrorCode::Protocol);
  }
}

TEST(JrpcClientMock, ANinthConnectionWaitsForASlot) {
  JrpcMockOptions o;
  o.connectionLimit = 1;
  JrpcMockServer mock(o);
  auto first = connectTo(mock);
  REQUIRE_OK(first);
  ClientOptions c = options();
  c.bannerTimeout = 150ms;
  auto second = connectTo(mock, c);
  REQUIRE_ERR(second, ErrorCode::Timeout);
  CHECK(second.error().message.find("8 connections") != std::string::npos);
  first->close();
  // The slot is free again.
  auto third = connectTo(mock);
  CHECK_OK(third);
}

TEST(JrpcClientMock, ConnectReachesTheMockOverLoopbackTcp) {
  JrpcMockServer mock;
  mock.registerFunction(0x82010000u, [](const JrpcCall &call) {
    return JrpcReturn::integer(static_cast<uint64_t>(call.args.at(0).integer + call.args.at(1).integer));
  });
  auto port = mock.listenTcp();
  if (!port) SKIP("loopback TCP refused: " + formatError(port.error()));
  net::Endpoint endpoint;
  endpoint.scheme = "jrpc";
  endpoint.host = "127.0.0.1";
  endpoint.port = *port;
  endpoint.timeout = 5000ms;
  auto client = JrpcClient::connect(endpoint, options());
  REQUIRE_OK(client);
  const auto peer = client->peer();
  REQUIRE(peer.has_value());
  CHECK_EQ(peer->host, std::string("127.0.0.1"));
  CHECK_EQ(peer->port, *port);
  auto r = client->call(jt::intCall());
  REQUIRE_OK(r);
  CHECK(r->value == CallValue(uint64_t{21}));
  // reconnect() uses the connector connect() made.
  REQUIRE_OK(client->reconnect());
  CHECK_OK(client->call(jt::intCall()));
  client->close();
  CHECK(mock.waitForActiveConnections(0));
}

TEST(JrpcClientMock, ConnectStopsAtOnceWhenTheTokenIsStopped) {
  std::stop_source source;
  source.request_stop();
  net::Endpoint endpoint;
  endpoint.scheme = "jrpc";
  endpoint.host = "127.0.0.1";
  endpoint.port = 1;
  CHECK_ERR(JrpcClient::connect(endpoint, options(), source.get_token()), ErrorCode::Cancelled);
}

TEST(JrpcClientMock, ConnectToAPortNobodyListensOnIsConnectFailed) {
  JrpcMockServer mock;
  auto port = mock.listenTcp();
  if (!port) SKIP("loopback TCP refused: " + formatError(port.error()));
  mock.stop();
  net::Endpoint endpoint;
  endpoint.scheme = "jrpc";
  endpoint.host = "127.0.0.1";
  endpoint.port = *port;
  endpoint.timeout = 2000ms;
  CHECK_ERR(JrpcClient::connect(endpoint, options()), ErrorCode::ConnectFailed);
}
