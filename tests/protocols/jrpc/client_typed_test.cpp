#include "protocols/jrpc/client_fake.hpp"
#include "support/jrpc_mock_server.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/jrpc/client.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The typed call helpers (callVoid ... callFloats): the return kind they put on the wire,
// how they decode and reinterpret the reply, the refusal of a spec that asks for another
// kind, and the errors they share with call(). First against the scripted fake with the
// command lines spelled out, then against the mock server's independent parser.

using namespace updclient;
using namespace updclient::jrpc;
using jt::FakeConsole;
using namespace std::chrono_literals;
using ut::JrpcCall;
using ut::JrpcMockServer;
using ut::JrpcReturn;

namespace {

// The value of a result that is checked to have one; a default value if it has none, so
// that a failure is reported instead of aborting the run.
template <class T> T got(const Result<T> &result) {
  CHECK_OK(result);
  return result ? *result : T{};
}

// `type` and `as` of a call to 0x82000000 without arguments.
std::string at(int type, int arraySize = 0) {
  return "consolefeatures ver=2 type=" + std::to_string(type) + " as=" + std::to_string(arraySize) +
         " params=\"A\\82000000\\A\\0\\\"";
}

CallSpec spec(ReturnKind returns = ReturnKind::Void, size_t arraySize = 0) {
  return jt::callAt(0x82000000, returns, {}, arraySize);
}

// A console that answers one command with `reply` (CR LF added).
std::shared_ptr<FakeConsole> answering(std::string_view reply) {
  auto console = FakeConsole::create();
  console->handle([text = std::string(reply)](FakeConsole &c, const std::string &) { c.line(text); });
  return console;
}

ClientOptions mockOptions() {
  ClientOptions o = jt::quickOptions();
  o.callTimeout = 3000ms;
  o.idleTimeout = 1000ms;
  return o;
}

} // namespace

// --- Against the scripted fake ------------------------------------------------------

TEST(JrpcClientTyped, EachHelperPutsItsKindOnTheWireAndDecodesTheReply) {
  auto console = FakeConsole::create();
  console->on(at(0), "S_OK\r\n")
      .on(at(1), "2A\r\n")
      .on(at(1), "2A\r\n")
      .on(at(4), "7F\r\n")
      .on(at(8), "248173A00\r\n")
      .on(at(8), "248173A00\r\n")
      .on(at(3), "2.500000\r\n")
      .on(at(2), "hello there\r\n")
      .on(at(7, 3), "1,FF,A;\r\n")
      .on(at(5, 2), "-1,2;\r\n")
      .on(at(6, 2), "0.500000,1.250000;\r\n");
  auto client = jt::connected(console);

  CHECK_OK(client.callVoid(spec()));
  auto i32 = client.callInt32(spec());
  REQUIRE_OK(i32);
  CHECK_EQ(*i32, int32_t{42});
  auto u32 = client.callUInt32(spec());
  REQUIRE_OK(u32);
  CHECK_EQ(*u32, uint32_t{42});
  auto byte = client.callByte(spec());
  REQUIRE_OK(byte);
  CHECK_EQ(*byte, uint8_t{0x7F});
  auto i64 = client.callInt64(spec());
  REQUIRE_OK(i64);
  CHECK_EQ(*i64, int64_t{0x248173A00});
  auto u64 = client.callUInt64(spec());
  REQUIRE_OK(u64);
  CHECK_EQ(*u64, uint64_t{0x248173A00});
  auto f = client.callFloat(spec());
  REQUIRE_OK(f);
  CHECK_EQ(*f, 2.5);
  auto text = client.callString(spec());
  REQUIRE_OK(text);
  CHECK_EQ(*text, std::string("hello there"));
  auto bytes = client.callBytes(spec(ReturnKind::Void, 3));
  REQUIRE_OK(bytes);
  CHECK(*bytes == std::vector<uint8_t>({1, 255, 10}));
  auto ints = client.callInts(spec(ReturnKind::Void, 2));
  REQUIRE_OK(ints);
  CHECK(*ints == std::vector<int32_t>({-1, 2}));
  auto floats = client.callFloats(spec(ReturnKind::Void, 2));
  REQUIRE_OK(floats);
  CHECK(*floats == std::vector<double>({0.5, 1.25}));

  CHECK_EQ(console->problems(), std::string());
  CHECK(client.isConnected());
}

TEST(JrpcClientTyped, ASpecThatNamesTheHelpersOwnKindIsAccepted) {
  auto console = FakeConsole::create();
  console->on(at(1), "5\r\n").on(at(7, 2), "1,2;\r\n").on(at(0), "0\r\n");
  auto client = jt::connected(console);
  CHECK_OK(client.callInt32(spec(ReturnKind::Int)));
  CHECK_OK(client.callBytes(spec(ReturnKind::ByteArray, 2)));
  CHECK_OK(client.callVoid(spec(ReturnKind::Void)));
  CHECK_EQ(console->problems(), std::string());
}

TEST(JrpcClientTyped, SignedAndUnsignedReadTheSameBits) {
  auto console = FakeConsole::create();
  console->on(at(1), "FFFFFFFE\r\n")
      .on(at(1), "FFFFFFFE\r\n")
      .on(at(1), "80000000\r\n")
      .on(at(1), "7FFFFFFF\r\n")
      .on(at(8), "FFFFFFFFFFFFFFFF\r\n")
      .on(at(8), "FFFFFFFFFFFFFFFF\r\n")
      .on(at(8), "8000000000000000\r\n")
      .on(at(4), "1FF\r\n");
  auto client = jt::connected(console);
  CHECK_EQ(got(client.callInt32(spec())), int32_t{-2});
  CHECK_EQ(got(client.callUInt32(spec())), uint32_t{0xFFFFFFFE});
  CHECK_EQ(got(client.callInt32(spec())), std::numeric_limits<int32_t>::min());
  CHECK_EQ(got(client.callInt32(spec())), std::numeric_limits<int32_t>::max());
  CHECK_EQ(got(client.callInt64(spec())), int64_t{-1});
  CHECK_EQ(got(client.callUInt64(spec())), std::numeric_limits<uint64_t>::max());
  CHECK_EQ(got(client.callInt64(spec())), std::numeric_limits<int64_t>::min());
  // Contract (J1): a byte reply keeps the low 8 bits of whatever hex arrives.
  CHECK_EQ(got(client.callByte(spec())), uint8_t{0xFF});
}

TEST(JrpcClientTyped, ArgumentsAndTargetsAreSentAsForCall) {
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "2A\r\n");
  console->on("consolefeatures ver=2 type=8 system module=\"xam.xex\" ord=436 as=0 params=\"A\\0\\A\\0\\\"", "248173A00\r\n");
  auto client = jt::connected(console);
  auto sum = client.callInt32(jt::callAt(0x82010000, ReturnKind::Void, {Arg::i32(5), Arg::i32(16)}));
  REQUIRE_OK(sum);
  CHECK_EQ(*sum, 42);

  CallSpec byOrdinal;
  byOrdinal.target = ByName{"xam.xex", 436};
  byOrdinal.thread = ThreadContext::System;
  auto big = client.callUInt64(byOrdinal);
  REQUIRE_OK(big);
  CHECK_EQ(*big, uint64_t{0x248173A00});
  CHECK_EQ(console->problems(), std::string());
}

TEST(JrpcClientTyped, ASpecForAnotherKindIsRefusedBeforeAnythingIsSent) {
  auto console = FakeConsole::create();
  auto client = jt::connected(console);
  const auto refused = [](const auto &result) {
    return !result.has_value() && result.error().code == ErrorCode::InvalidArgument;
  };
  CHECK(refused(client.callVoid(spec(ReturnKind::Int))));
  CHECK(refused(client.callInt32(spec(ReturnKind::String))));
  CHECK(refused(client.callUInt32(spec(ReturnKind::Byte))));
  CHECK(refused(client.callUInt64(spec(ReturnKind::Int))));
  CHECK(refused(client.callBytes(spec(ReturnKind::IntArray, 2))));
  CHECK(refused(client.callFloat(spec(ReturnKind::Byte))));
  CHECK(refused(client.callInts(spec(ReturnKind::ByteArray, 2))));
  auto detail = client.callInt32(spec(ReturnKind::String));
  REQUIRE_ERR(detail, ErrorCode::InvalidArgument);
  CHECK(detail.error().message.find("String") != std::string::npos);
  CHECK(detail.error().message.find("Int") != std::string::npos);

  CHECK_EQ(console->written(), size_t{0});
  CHECK(client.isConnected());
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK_EQ(delivery->command, std::string("call"));
  CHECK(delivery->delivery == Delivery::NotSent);
}

TEST(JrpcClientTyped, TheArraySizeMustSuitTheKind) {
  auto console = FakeConsole::create();
  auto client = jt::connected(console);
  // A scalar helper with an array size, an array helper without one or with too many.
  CHECK_ERR(client.callInt32(spec(ReturnKind::Void, 3)), ErrorCode::InvalidArgument);
  CHECK_ERR(client.callString(spec(ReturnKind::Void, 1)), ErrorCode::InvalidArgument);
  CHECK_ERR(client.callBytes(spec(ReturnKind::Void, 0)), ErrorCode::InvalidArgument);
  CHECK_ERR(client.callInts(spec(ReturnKind::Void, 9)), ErrorCode::LimitExceeded);
  CHECK_ERR(client.callFloats(spec(ReturnKind::Void, 0)), ErrorCode::InvalidArgument);
  CHECK_EQ(console->written(), size_t{0});
  CHECK(client.isConnected());
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
}

TEST(JrpcClientTyped, BadSpecsAreRefusedLikeCall) {
  auto console = FakeConsole::create();
  auto client = jt::connected(console);
  std::vector<Arg> many(38, Arg::i32(1));
  CHECK_ERR(client.callVoid(jt::callAt(0x82000000, ReturnKind::Void, many)), ErrorCode::LimitExceeded);
  CHECK_ERR(client.callInt32(jt::callAt(0, ReturnKind::Void)), ErrorCode::InvalidArgument);
  CHECK_ERR(client.callFloat(jt::callAt(0x82000000, ReturnKind::Void, {Arg::f64(std::nan(""))})), ErrorCode::InvalidArgument);
  CHECK_EQ(console->written(), size_t{0});
}

TEST(JrpcClientTyped, AnErrorLineIsARemoteFaultAndTheConnectionStays) {
  auto console = FakeConsole::create();
  console->on(at(1), "error=Could not resolve function address, params = x, 1\r\n").on(at(1), "7\r\n");
  auto client = jt::connected(console);
  auto r = client.callInt32(spec());
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(remoteFault(r.error()) == std::optional<RemoteFault>(RemoteFault::CouldNotResolve));
  CHECK(client.isConnected());
  CHECK(client.lastDelivery()->delivery == Delivery::Answered);
  CHECK_EQ(got(client.callInt32(spec())), 7);
}

TEST(JrpcClientTyped, EveryHelperClosesTheConnectionOnAReplyOfTheWrongShape) {
  struct Row {
    const char *name;
    int type;
    size_t arraySize;
    const char *reply;
    std::function<bool(JrpcClient &)> run;
  };
  const std::vector<Row> rows = {
      {"callVoid", 0, 0, "hello", [](JrpcClient &c) { return c.callVoid(spec()).has_value(); }},
      {"callInt32", 1, 0, "hello", [](JrpcClient &c) { return c.callInt32(spec()).has_value(); }},
      {"callUInt32", 1, 0, "123456789", [](JrpcClient &c) { return c.callUInt32(spec()).has_value(); }},
      {"callByte", 4, 0, "", [](JrpcClient &c) { return c.callByte(spec()).has_value(); }},
      {"callInt64", 8, 0, "-1", [](JrpcClient &c) { return c.callInt64(spec()).has_value(); }},
      {"callUInt64", 8, 0, "12345678901234567", [](JrpcClient &c) { return c.callUInt64(spec()).has_value(); }},
      {"callFloat", 3, 0, "x", [](JrpcClient &c) { return c.callFloat(spec()).has_value(); }},
      {"callBytes", 7, 2, "1,2,3;", [](JrpcClient &c) { return c.callBytes(spec(ReturnKind::Void, 2)).has_value(); }},
      {"callInts", 5, 2, "1,2", [](JrpcClient &c) { return c.callInts(spec(ReturnKind::Void, 2)).has_value(); }},
      {"callFloats", 6, 2, "1.0;", [](JrpcClient &c) { return c.callFloats(spec(ReturnKind::Void, 2)).has_value(); }},
  };
  for (const Row &row : rows) {
    auto console = FakeConsole::create();
    console->on(at(row.type, static_cast<int>(row.arraySize)), std::string(row.reply) + "\r\n");
    auto client = jt::connected(console);
    CHECK_MSG(!row.run(client), row.name);
    CHECK_MSG(!client.isConnected(), row.name);
    CHECK_MSG(console->closed(), row.name);
    CHECK_MSG(console->byes() == 0, row.name);
  }
}

TEST(JrpcClientTyped, ADebugLineOrASilentConsoleEndsTheConnectionToo) {
  {
    auto client = jt::connected(answering("DEBUG: no JRPC"));
    CHECK_ERR(client.callString(spec()), ErrorCode::Unsupported);
    CHECK(!client.isConnected());
  }
  {
    auto console = FakeConsole::create();
    console->on(at(1), "");
    ClientOptions options = jt::quickOptions();
    options.callTimeout = 100ms;
    auto client = jt::connected(console, options);
    auto r = client.callUInt32(spec());
    REQUIRE_ERR(r, ErrorCode::Timeout);
    CHECK(r.error().message.find("may have carried it out") != std::string::npos);
    CHECK(!client.isConnected());
    CHECK(client.lastDelivery()->delivery == Delivery::Sent);
  }
}

TEST(JrpcClientTyped, AClosedClientIsNotConnectedForEveryHelper) {
  auto console = FakeConsole::create();
  auto client = jt::connected(console);
  client.close();
  CHECK_ERR(client.callVoid(spec()), ErrorCode::NotConnected);
  CHECK_ERR(client.callInt32(spec()), ErrorCode::NotConnected);
  CHECK_ERR(client.callString(spec()), ErrorCode::NotConnected);
  CHECK_ERR(client.callBytes(spec(ReturnKind::Void, 2)), ErrorCode::NotConnected);
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);

  JrpcClient moved = std::move(client);
  CHECK_ERR(client.callInt32(spec()), ErrorCode::NotConnected);
}

TEST(JrpcClientTyped, ACallFromTheTraceHookIsRefused) {
  JrpcClient *self = nullptr;
  std::vector<Result<int32_t>> inner;
  ClientOptions options = jt::quickOptions();
  options.trace = [&](TraceEvent event, std::string_view, uint64_t) {
    if (event == TraceEvent::Sent && self) inner.push_back(self->callInt32(spec()));
  };
  auto console = FakeConsole::create();
  console->on(at(1), "2A\r\n");
  auto client = jt::connected(console, options);
  self = &client;
  auto r = client.callInt32(spec());
  self = nullptr;
  REQUIRE_OK(r);
  CHECK_EQ(*r, 42);
  REQUIRE_EQ(inner.size(), size_t{1});
  CHECK_ERR(inner[0], ErrorCode::InvalidArgument);
  CHECK_EQ(console->commands().size(), size_t{1});
}

// --- Against the mock server ---------------------------------------------------------

TEST(JrpcTypedMock, EveryHelperReadsWhatTheMockFormats) {
  JrpcMockServer mock;
  mock.registerFunction(0x82000000u, [](const JrpcCall &call) {
    switch (call.type) {
    case 0: return JrpcReturn::integer(0x1234);
    case 1: return JrpcReturn::integer(0xFFFFFFFEull);
    case 2: return JrpcReturn::string("a console string");
    case 3: return JrpcReturn::real(-1.5);
    case 4: return JrpcReturn::integer(0xAB);
    case 5: return JrpcReturn::intArray({7, -8, 9});
    case 6: return JrpcReturn::floatArray({0.25, -2.0});
    case 7: return JrpcReturn::byteArray({0, 0x7F, 0xFF, 1});
    default: return JrpcReturn::integer(0xFFFFFFFFFFFFFFFEull);
    }
  });
  auto client = JrpcClient::open(mock.connector(), mockOptions());
  REQUIRE_OK(client);

  CHECK_OK(client->callVoid(spec()));
  CHECK_EQ(got(client->callInt32(spec())), int32_t{-2});
  CHECK_EQ(got(client->callUInt32(spec())), uint32_t{0xFFFFFFFE});
  CHECK_EQ(got(client->callString(spec())), std::string("a console string"));
  CHECK_EQ(got(client->callFloat(spec())), -1.5);
  CHECK_EQ(got(client->callByte(spec())), uint8_t{0xAB});
  CHECK(got(client->callInts(spec(ReturnKind::Void, 3))) == std::vector<int32_t>({7, -8, 9}));
  CHECK(got(client->callFloats(spec(ReturnKind::Void, 2))) == std::vector<double>({0.25, -2.0}));
  CHECK(got(client->callBytes(spec(ReturnKind::Void, 4))) == std::vector<uint8_t>({0, 0x7F, 0xFF, 1}));
  CHECK_EQ(got(client->callInt64(spec())), int64_t{-2});
  CHECK_EQ(got(client->callUInt64(spec())), uint64_t{0xFFFFFFFFFFFFFFFEull});
  CHECK(client->isConnected());
  CHECK_EQ(mock.calls().size(), size_t{11});
}

TEST(JrpcTypedMock, TheMockSeesTheKindTheHelperChose) {
  JrpcMockServer mock;
  mock.registerFunction(0x82000000u, [](const JrpcCall &) { return JrpcReturn::integer(1); });
  auto client = JrpcClient::open(mock.connector(), mockOptions());
  REQUIRE_OK(client);
  CHECK_OK(client->callVoid(spec()));
  CHECK_OK(client->callInt32(spec()));
  CHECK_OK(client->callByte(spec()));
  CHECK_OK(client->callUInt64(spec()));
  const auto calls = mock.calls();
  REQUIRE_EQ(calls.size(), size_t{4});
  CHECK_EQ(calls[0].type, 0);
  CHECK_EQ(calls[1].type, 1);
  CHECK_EQ(calls[2].type, 4);
  CHECK_EQ(calls[3].type, 8);
}

TEST(JrpcTypedMock, ArgumentsArriveWithTheirValues) {
  JrpcMockServer mock;
  mock.registerFunction(0x82010000u, [](const JrpcCall &call) {
    return JrpcReturn::integer(static_cast<uint64_t>(call.args.at(0).integer + call.args.at(1).integer));
  });
  auto client = JrpcClient::open(mock.connector(), mockOptions());
  REQUIRE_OK(client);
  auto sum = client->callInt32(jt::callAt(0x82010000, ReturnKind::Void, {Arg::i32(-5), Arg::u32(0xFFFFFFF0u)}));
  REQUIRE_OK(sum);
  // -5 plus the uint32 0xFFFFFFF0 sent as the int -16.
  CHECK_EQ(*sum, -21);
}

TEST(JrpcTypedMock, AnUnknownFunctionIsAFaultOfTheHelperToo) {
  JrpcMockServer mock;
  auto client = JrpcClient::open(mock.connector(), mockOptions());
  REQUIRE_OK(client);
  auto r = client->callString(jt::callAt(0x83000000, ReturnKind::Void));
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(remoteFault(r.error()) == std::optional<RemoteFault>(RemoteFault::CouldNotResolve));
  CHECK(client->isConnected());
}
