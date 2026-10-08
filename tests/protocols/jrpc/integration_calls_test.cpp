#include "protocols/jrpc/integration_support.hpp"

#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <optional>
#include <variant>

// Generic calls and system opcodes, the client against the mock server (section 4 of
// docs/JRPC_PROTOCOL.md): every return kind, every argument kind, 37 arguments, the
// largest line, a hundred calls on one connection, every opcode. The mock's parser shares
// no code with the client's builder, so a line the client builds wrongly is not understood
// and a reply the mock formats the way the server does has to be read by the client.

using namespace jit;
using namespace updclient::jrpc;

namespace {

constexpr uint32_t kFn = 0x82010000u;
constexpr uint32_t kFn2 = 0x82020000u;

// One function that answers by the `type` it was called with.
JrpcReturn byType(const JrpcCall &call) {
  switch (call.type) {
  case 0: return JrpcReturn::integer(0x1234);
  case 1: return JrpcReturn::integer(0x1FFFFFFFEull);
  case 2: return JrpcReturn::string("Hello, w\xC3\xB6rld = 100% \"quoted\" \\ back");
  case 3: return JrpcReturn::real(-2.5);
  case 4: return JrpcReturn::integer(0x1AB);
  case 5: return JrpcReturn::intArray({INT32_MIN, -1, 0, 7, INT32_MAX, 100, -100, 12345678});
  case 6: return JrpcReturn::floatArray({1.5, -2.25, 0.0, 1000000.125, -0.5, 3.0, 4.0, 5.0});
  case 7: return JrpcReturn::byteArray({0x00, 0x01, 0x7F, 0x80, 0xFF, 0x10, 0xAB, 0xCD});
  default: return JrpcReturn::integer(0xFEDCBA9876543210ull);
  }
}

// A call by address whose line is exactly `target` bytes long, built from one string
// argument and, when the parity needs it, a second integer argument of another width.
std::optional<CallSpec> specOfLength(size_t target) {
  for (const int32_t pad : {0, 10, 100}) {
    // Every character of the string costs two hex digits; start near the answer.
    auto base = buildCommand(callAt(kFn, ReturnKind::Int, {Arg::i32(pad), Arg::string("")}));
    if (!base || base->size() >= target) continue;
    const size_t estimate = (target - base->size()) / 2;
    for (size_t n = estimate > 8 ? estimate - 8 : 0; n <= estimate + 8; ++n) {
      CallSpec spec = callAt(kFn, ReturnKind::Int, {Arg::i32(pad), Arg::string(std::string(n, 'a'))});
      auto line = buildCommand(spec);
      if (line && line->size() == target) return spec;
    }
  }
  return std::nullopt;
}

} // namespace

// --- Return kinds ----------------------------------------------------------------------

JRPC_LINK_TEST(JrpcIntegration, EveryReturnKindIsReadFromTheMocksReply) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  auto client = rig.client();

  auto v = client.call(callAt(kFn, ReturnKind::Void));
  REQUIRE_OK(v);
  CHECK(std::holds_alternative<std::monostate>(v->value));
  CHECK_EQ(v->line, std::string("1234"));

  auto i = client.call(callAt(kFn, ReturnKind::Int));
  REQUIRE_OK(i);
  CHECK(i->value == CallValue(uint64_t{0xFFFFFFFE}));
  CHECK_EQ(i->line, std::string("FFFFFFFE"));

  auto s = client.call(callAt(kFn, ReturnKind::String));
  REQUIRE_OK(s);
  CHECK(s->value == CallValue(std::string("Hello, w\xC3\xB6rld = 100% \"quoted\" \\ back")));

  auto f = client.call(callAt(kFn, ReturnKind::Float));
  REQUIRE_OK(f);
  CHECK(f->value == CallValue(-2.5));
  CHECK_EQ(f->line, std::string("-2.500000"));

  auto b = client.call(callAt(kFn, ReturnKind::Byte));
  REQUIRE_OK(b);
  CHECK(b->value == CallValue(uint64_t{0xAB}));

  auto ints = client.call(callAt(kFn, ReturnKind::IntArray, {}, 8));
  REQUIRE_OK(ints);
  CHECK(ints->value == CallValue(std::vector<int32_t>{INT32_MIN, -1, 0, 7, INT32_MAX, 100, -100, 12345678}));

  auto floats = client.call(callAt(kFn, ReturnKind::FloatArray, {}, 8));
  REQUIRE_OK(floats);
  CHECK(floats->value == CallValue(std::vector<double>{1.5, -2.25, 0.0, 1000000.125, -0.5, 3.0, 4.0, 5.0}));

  auto bytes = client.call(callAt(kFn, ReturnKind::ByteArray, {}, 8));
  REQUIRE_OK(bytes);
  CHECK(bytes->value == CallValue(std::vector<uint8_t>{0x00, 0x01, 0x7F, 0x80, 0xFF, 0x10, 0xAB, 0xCD}));

  auto wide = client.call(callAt(kFn, ReturnKind::Int64));
  REQUIRE_OK(wide);
  CHECK(wide->value == CallValue(uint64_t{0xFEDCBA9876543210ull}));
  CHECK_EQ(wide->line, std::string("FEDCBA9876543210"));

  CHECK(client.isConnected());
  const auto calls = rig.mock.calls();
  REQUIRE_EQ(calls.size(), size_t{9});
  const int types[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  for (size_t k = 0; k < 9; ++k) CHECK_EQ(calls[k].type, types[k]);
  CHECK_EQ(calls[5].arraySize, 8);
  rig.checkCleanTraffic();
}

JRPC_LINK_TEST(JrpcIntegration, TheTypedHelpersDecodeEveryKind) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  auto client = rig.client();

  CHECK_OK(client.callVoid(callAt(kFn)));
  CHECK_EQ(got(client.callInt32(callAt(kFn))), int32_t{-2});
  CHECK_EQ(got(client.callUInt32(callAt(kFn))), uint32_t{0xFFFFFFFE});
  CHECK_EQ(got(client.callByte(callAt(kFn))), uint8_t{0xAB});
  CHECK_EQ(got(client.callInt64(callAt(kFn))), static_cast<int64_t>(0xFEDCBA9876543210ull));
  CHECK_EQ(got(client.callUInt64(callAt(kFn))), uint64_t{0xFEDCBA9876543210ull});
  CHECK_EQ(got(client.callFloat(callAt(kFn))), -2.5);
  CHECK_EQ(got(client.callString(callAt(kFn))), std::string("Hello, w\xC3\xB6rld = 100% \"quoted\" \\ back"));
  CHECK_EQ(got(client.callInts(callAt(kFn, ReturnKind::Void, {}, 3))), (std::vector<int32_t>{INT32_MIN, -1, 0}));
  CHECK_EQ(got(client.callFloats(callAt(kFn, ReturnKind::Void, {}, 2))), (std::vector<double>{1.5, -2.25}));
  CHECK_EQ(got(client.callBytes(callAt(kFn, ReturnKind::Void, {}, 5))), (std::vector<uint8_t>{0, 1, 0x7F, 0x80, 0xFF}));
  CHECK(client.isConnected());

  // A helper does not take a spec for another kind, and nothing is sent for it.
  const size_t before = rig.mock.calls().size();
  CHECK_ERR(client.callString(callAt(kFn, ReturnKind::Int)), ErrorCode::InvalidArgument);
  CHECK_ERR(client.callInts(callAt(kFn, ReturnKind::Void, {}, 0)), ErrorCode::InvalidArgument);
  CHECK_EQ(rig.mock.calls().size(), before);
  CHECK(client.isConnected());
  rig.checkCleanTraffic();
}

JRPC_LINK_TEST(JrpcIntegration, AnArrayShorterThanRequestedIsPaddedWithZerosByTheConsole) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &call) {
    if (call.type == 5) return JrpcReturn::intArray({9, 8});
    if (call.type == 6) return JrpcReturn::floatArray({0.5});
    return JrpcReturn::byteArray({0xEE});
  });
  auto client = rig.client();
  CHECK_EQ(got(client.callInts(callAt(kFn, ReturnKind::Void, {}, 4))), (std::vector<int32_t>{9, 8, 0, 0}));
  CHECK_EQ(got(client.callFloats(callAt(kFn, ReturnKind::Void, {}, 3))), (std::vector<double>{0.5, 0.0, 0.0}));
  CHECK_EQ(got(client.callBytes(callAt(kFn, ReturnKind::Void, {}, 2))), (std::vector<uint8_t>{0xEE, 0}));
}

JRPC_LINK_TEST(JrpcIntegration, EveryArraySizeFromOneToEightIsExact) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  auto client = rig.client();
  const std::vector<int32_t> ints = {INT32_MIN, -1, 0, 7, INT32_MAX, 100, -100, 12345678};
  const std::vector<uint8_t> bytes = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0x10, 0xAB, 0xCD};
  for (size_t n = 1; n <= 8; ++n) {
    auto a = client.callInts(callAt(kFn, ReturnKind::Void, {}, n));
    auto b = client.callBytes(callAt(kFn, ReturnKind::Void, {}, n));
    auto c = client.callFloats(callAt(kFn, ReturnKind::Void, {}, n));
    REQUIRE_OK(a);
    REQUIRE_OK(b);
    REQUIRE_OK(c);
    CHECK_EQ(*a, std::vector<int32_t>(ints.begin(), ints.begin() + static_cast<std::ptrdiff_t>(n)));
    CHECK_EQ(*b, std::vector<uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n)));
    CHECK_EQ(c->size(), n);
  }
  // Nine is refused by the client before anything goes out.
  const size_t before = rig.mock.calls().size();
  CHECK_ERR(client.callInts(callAt(kFn, ReturnKind::Void, {}, 9)), ErrorCode::LimitExceeded);
  CHECK_ERR(client.call(callAt(kFn, ReturnKind::Uint64Array, {}, 2)), ErrorCode::Unsupported);
  CHECK_EQ(rig.mock.calls().size(), before);
  CHECK(client.isConnected());
}

// Open question 1: what the server does with `as` above 8. Both readings, with the
// client's limit raised, and the default limit that makes the question moot.
JRPC_LINK_TEST(JrpcIntegration, MoreThanEightElementsDependOnWhatTheConsoleDoes) {
  ClientOptions wide = quickOptions();
  wide.maxArrayElements = 12;
  {
    // The format loops: twelve elements come back.
    JrpcMockOptions o;
    o.arrayOverflow = ut::JrpcArrayOverflow::Loop;
    Rig rig(link, o);
    rig.mock.registerFunction(kFn, [](const JrpcCall &) { return JrpcReturn::intArray({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}); });
    auto client = rig.client(wide);
    auto r = client.callInts(callAt(kFn, ReturnKind::Void, {}, 12));
    REQUIRE_OK(r);
    CHECK_EQ(r->size(), size_t{12});
    CHECK(client.isConnected());
  }
  {
    // Eight slots only: the reply has fewer elements than were asked for, which the client
    // does not read as an answer, and the connection is not reused.
    Rig rig(link);
    rig.mock.registerFunction(kFn, [](const JrpcCall &) { return JrpcReturn::intArray({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}); });
    auto client = rig.client(wide);
    auto r = client.callInts(callAt(kFn, ReturnKind::Void, {}, 12));
    REQUIRE_ERR(r, ErrorCode::Protocol);
    CHECK(!client.isConnected());
    // Within the limit of the server it is fine.
    REQUIRE_OK(client.reconnect());
    CHECK_EQ(got(client.callInts(callAt(kFn, ReturnKind::Void, {}, 8))).size(), size_t{8});
  }
}

JRPC_LINK_TEST(JrpcIntegration, VoidAndByteRepliesInTheirTwoSpellings) {
  for (const auto answer : {ut::JrpcVoidAnswer::Hex, ut::JrpcVoidAnswer::SOk}) {
    for (const bool padded : {false, true}) {
      JrpcMockOptions o;
      o.voidAnswer = answer;
      o.byteReplyPadded = padded;
      Rig rig(link, o);
      rig.mock.registerFunction(kFn, [](const JrpcCall &call) {
        return call.type == 4 ? JrpcReturn::integer(0x05) : JrpcReturn::integer(0);
      });
      auto client = rig.client();
      CHECK_OK(client.callVoid(callAt(kFn)));
      CHECK_EQ(got(client.callByte(callAt(kFn))), uint8_t{5});
      const auto calls = rig.mock.calls();
      REQUIRE_EQ(calls.size(), size_t{2});
      CHECK(client.isConnected());
    }
  }
}

JRPC_LINK_TEST(JrpcIntegration, FloatsKeepSixDecimalsAndLargeValuesSurvive) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &call) {
    const double values[] = {0.0, 0.1, -0.000001, 1e10, 123456.789012, 3.4028234e38, -1.5e-7};
    return JrpcReturn::real(values[static_cast<size_t>(call.args.at(0).integer)]);
  });
  auto client = rig.client();
  const double expected[] = {0.0, 0.1, -0.000001, 1e10, 123456.789012, 340282340000000000000000000000000000000.0, -0.0};
  for (int k = 0; k < 7; ++k) {
    auto r = client.callFloat(callAt(kFn, ReturnKind::Float, {Arg::i32(k)}));
    REQUIRE_OK(r);
    // Six decimals is all the console prints.
    CHECK_MSG(std::fabs(*r - expected[k]) <= 5e-7 + std::fabs(expected[k]) * 1e-15,
              "value " + std::to_string(k) + " read as " + std::to_string(*r));
  }
  CHECK(client.isConnected());
}

JRPC_LINK_TEST(JrpcIntegration, StringsOfAnyLengthUpToTheReplyLimit) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &call) {
    return JrpcReturn::string(std::string(static_cast<size_t>(call.args.at(0).integer), 'x'));
  });
  ClientOptions o = quickOptions();
  o.maxReplyBytes = 1000;
  auto client = rig.client(o);
  for (const int n : {0, 1, 999, 1000}) {
    auto r = client.callString(callAt(kFn, ReturnKind::String, {Arg::i32(n)}));
    REQUIRE_OK(r);
    CHECK_EQ(r->size(), static_cast<size_t>(n));
  }
  // One more byte than maxReplyBytes ends the connection.
  auto r = client.callString(callAt(kFn, ReturnKind::String, {Arg::i32(1001)}));
  CHECK_ERR(r, ErrorCode::LimitExceeded);
  CHECK(!client.isConnected());

  // The default limit takes a 60 KB string.
  Rig big(link);
  big.mock.registerFunction(kFn, [](const JrpcCall &) { return JrpcReturn::string(std::string(60000, 'y')); });
  auto bigClient = big.client();
  CHECK_EQ(got(bigClient.callString(callAt(kFn, ReturnKind::String))).size(), size_t{60000});
}

// A reply is classified by its text before its kind: these two are part of the contract
// (D3, D4) and the only strings a function cannot return.
JRPC_LINK_TEST(JrpcIntegration, StringRepliesThatLookLikeFaultsAreTreatedAsFaults) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &call) {
    return JrpcReturn::string(call.args.at(0).integer == 0 ? "error=not really" : "Debugging is off, DEBUG is on");
  });
  auto client = rig.client();
  auto e = client.callString(callAt(kFn, ReturnKind::String, {Arg::i32(0)}));
  REQUIRE_ERR(e, ErrorCode::Io);
  CHECK(remoteFault(e.error()).has_value());
  CHECK(client.isConnected());
  auto d = client.callString(callAt(kFn, ReturnKind::String, {Arg::i32(1)}));
  CHECK_ERR(d, ErrorCode::Unsupported);
  CHECK(!client.isConnected());
}

// --- Arguments -------------------------------------------------------------------------

JRPC_LINK_TEST(JrpcIntegration, EveryArgumentKindReachesTheFunctionDecoded) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &) { return JrpcReturn::integer(1); });
  auto client = rig.client();
  const std::string text = "caf\xC3\xA9 \"x\" \\ 100%";
  const std::vector<uint8_t> blob = {0x00, 0xFF, 0x10, 0x80};
  CallSpec spec = callAt(kFn, ReturnKind::Int,
                         {Arg::i32(-5), Arg::u32(0xFFFFFFFFu), Arg::u32(0x80000000u), Arg::boolean(true),
                          Arg::boolean(false), Arg::byte(200), Arg::i64(INT64_MIN), Arg::u64(UINT64_MAX),
                          Arg::i64(0x123456789ABCull), Arg::f32(1.5f), Arg::f64(-0.25), Arg::string(text),
                          Arg::string(""), Arg::bytes(blob), Arg::bytes({}), Arg::ints({1, -2, 0x7FFFFFFF}),
                          Arg::floats({1.0f, -2.5f})});
  REQUIRE_OK(client.call(spec));
  const auto calls = rig.mock.calls();
  REQUIRE_EQ(calls.size(), size_t{1});
  const auto &a = calls[0].args;
  REQUIRE_EQ(a.size(), size_t{17});
  CHECK_EQ(a[0].tag, '1');
  CHECK_EQ(a[0].integer, int64_t{-5});
  CHECK_EQ(a[1].tag, '1');
  CHECK_EQ(a[1].u32(), uint32_t{0xFFFFFFFF});
  CHECK_EQ(a[2].u32(), uint32_t{0x80000000});
  CHECK_EQ(a[3].integer, int64_t{1});
  CHECK_EQ(a[4].integer, int64_t{0});
  CHECK_EQ(a[5].tag, '4');
  CHECK_EQ(a[5].integer, int64_t{200});
  CHECK_EQ(a[6].tag, '8');
  CHECK_EQ(a[6].integer, INT64_MIN);
  CHECK_EQ(static_cast<uint64_t>(a[7].integer), UINT64_MAX);
  CHECK_EQ(a[8].integer, int64_t{0x123456789ABC});
  CHECK_EQ(a[9].tag, '3');
  CHECK_EQ(a[9].real, 1.5);
  CHECK_EQ(a[10].real, -0.25);
  CHECK_EQ(a[11].tag, '2');
  CHECK_EQ(a[11].text(), text);
  CHECK_EQ(a[12].text(), std::string());
  CHECK_EQ(a[13].tag, '7');
  CHECK(a[13].data == blob);
  CHECK(a[14].data.empty());
  // Arrays go out as big-endian blobs (tag 7).
  CHECK_EQ(a[15].tag, '7');
  CHECK(a[15].data == (Bytes{0, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0xFE, 0x7F, 0xFF, 0xFF, 0xFF}));
  CHECK(a[16].data == (Bytes{0x3F, 0x80, 0, 0, 0xC0, 0x20, 0, 0}));
  CHECK(client.isConnected());
}

JRPC_LINK_TEST(JrpcIntegration, ThirtySevenArgumentsArriveAndThirtyEightAreRefused) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &call) {
    int64_t sum = 0;
    for (const auto &arg : call.args) sum += arg.integer;
    return JrpcReturn::integer(static_cast<uint64_t>(sum));
  });
  auto client = rig.client();

  std::vector<Arg> args;
  int sum = 0;
  for (int i = 0; i < 37; ++i) {
    args.push_back(Arg::i32(i * 3 - 20));
    sum += i * 3 - 20;
  }
  auto r = client.callInt32(callAt(kFn, ReturnKind::Int, args));
  REQUIRE_OK(r);
  CHECK_EQ(*r, sum);
  auto calls = rig.mock.calls();
  REQUIRE_EQ(calls.size(), size_t{1});
  REQUIRE_EQ(calls[0].args.size(), size_t{37});
  for (int i = 0; i < 37; ++i) CHECK_EQ(calls[0].args[static_cast<size_t>(i)].integer, int64_t{i * 3 - 20});

  // 37 arguments of every width at once.
  std::vector<Arg> mixed;
  for (int i = 0; i < 37; ++i) {
    switch (i % 5) {
    case 0: mixed.push_back(Arg::i32(i)); break;
    case 1: mixed.push_back(Arg::string("s" + std::to_string(i))); break;
    case 2: mixed.push_back(Arg::i64(i)); break;
    case 3: mixed.push_back(Arg::bytes({static_cast<uint8_t>(i)})); break;
    default: mixed.push_back(Arg::byte(static_cast<uint8_t>(i))); break;
    }
  }
  CHECK_OK(client.call(callAt(kFn, ReturnKind::Int, mixed)));
  calls = rig.mock.calls();
  REQUIRE_EQ(calls.size(), size_t{2});
  CHECK_EQ(calls[1].args.size(), size_t{37});

  // 38 are refused before anything is sent, and the connection is still good.
  args.push_back(Arg::i32(0));
  CHECK_ERR(client.call(callAt(kFn, ReturnKind::Int, args)), ErrorCode::LimitExceeded);
  CHECK_EQ(rig.mock.calls().size(), size_t{2});
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
  CHECK(client.isConnected());
  CHECK_OK(client.callInt32(callAt(kFn, ReturnKind::Int, {Arg::i32(1)})));
  rig.checkCleanTraffic();
}

JRPC_LINK_TEST(JrpcIntegration, TheLargestLineTheServerTakesArrivesIntact) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &call) {
    return JrpcReturn::integer(call.args.at(1).data.size());
  });
  auto client = rig.client();

  for (const size_t length : {size_t{8191}, size_t{8190}, size_t{8189}}) {
    auto spec = specOfLength(length);
    REQUIRE(spec.has_value());
    auto line = buildCommand(*spec);
    REQUIRE_OK(line);
    REQUIRE_EQ(line->size(), length);
    auto r = client.callUInt32(*spec);
    REQUIRE_OK(r);
    // The string argument's bytes arrived whole.
    const auto calls = rig.mock.calls();
    REQUIRE(!calls.empty());
    CHECK_EQ(*r, calls.back().args.at(1).data.size());
    CHECK(client.isConnected());
  }
  // The mock saw an 8193-byte line (with CR LF) for the 8191 one.
  const auto records = rig.recordsNamed("call");
  REQUIRE_EQ(records.size(), size_t{3});
  CHECK_EQ(records[0].length, size_t{8193});
  CHECK(!records[0].overLong);

  // One byte more is refused by the client; the mock never sees it.
  auto tooLong = specOfLength(8191);
  REQUIRE(tooLong.has_value());
  tooLong->args.push_back(Arg::i32(0));
  auto over = client.call(*tooLong);
  CHECK_ERR(over, ErrorCode::LimitExceeded);
  CHECK_EQ(rig.recordsNamed("call").size(), size_t{3});
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
  CHECK(client.isConnected());
  CHECK_OK(client.callUInt32(callAt(kFn, ReturnKind::Int, {Arg::i32(0), Arg::string("ok")})));
  rig.checkCleanTraffic();
}

JRPC_LINK_TEST(JrpcIntegration, ABlobAtTheLimitOfTheLineArrivesIntact) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &call) { return JrpcReturn::integer(call.args.at(0).data.size()); });
  auto client = rig.client();
  // `consolefeatures ver=2 type=1 as=0 params="A\82010000\A\1\7/n\<hex>\"` leaves the rest to the blob.
  Bytes blob = ut::patternBytes(3900, 3);
  auto r = client.callUInt32(callAt(kFn, ReturnKind::Int, {Arg::bytes(blob)}));
  REQUIRE_OK(r);
  CHECK_EQ(*r, uint32_t{3900});
  const auto calls = rig.mock.calls();
  REQUIRE_EQ(calls.size(), size_t{1});
  CHECK(calls[0].args.at(0).data == blob);
}

// --- Targets ---------------------------------------------------------------------------

JRPC_LINK_TEST(JrpcIntegration, CallsByModuleAndOrdinalReachTheExportInEitherThreadContext) {
  Rig rig(link);
  const uint32_t address = rig.mock.registerFunction("xam.xex", 436, [](const JrpcCall &call) {
    return JrpcReturn::integer(call.system ? 2 : 1);
  });
  auto client = rig.client();
  CallSpec spec;
  spec.target = ByName{"xam.xex", 436};
  spec.returns = ReturnKind::Int;
  spec.thread = ThreadContext::Title;
  CHECK_EQ(got(client.callUInt32(spec)), uint32_t{1});
  spec.thread = ThreadContext::System;
  CHECK_EQ(got(client.callUInt32(spec)), uint32_t{2});
  // The same function by the address ResolveFunction gave.
  auto resolved = client.resolveFunction("xam.xex", 436);
  REQUIRE_OK(resolved);
  CHECK_EQ(*resolved, address);
  CHECK_EQ(got(client.callUInt32(callAt(*resolved, ReturnKind::Int))), uint32_t{1});

  const auto calls = rig.mock.calls();
  REQUIRE_EQ(calls.size(), size_t{3});
  CHECK(calls[0].module == std::optional<std::string>("xam.xex"));
  CHECK_EQ(calls[0].ordinal, uint32_t{436});
  CHECK_EQ(calls[0].address, address);
  CHECK(!calls[0].system);
  CHECK(calls[1].system);
  CHECK(!calls[2].module.has_value());
}

JRPC_LINK_TEST(JrpcIntegration, AFunctionThatIsNotThereIsARemoteFaultAndTheConnectionStays) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &call) {
    return call.args.empty() ? JrpcReturn::integer(1) : JrpcReturn::failure("Version mismatch");
  });
  auto client = rig.client();
  auto missing = client.call(callAt(0x83000000, ReturnKind::Int));
  REQUIRE_ERR(missing, ErrorCode::Io);
  CHECK(remoteFault(missing.error()) == std::optional<RemoteFault>(RemoteFault::CouldNotResolve));
  CHECK(client.lastDelivery()->delivery == Delivery::Answered);

  CallSpec byName;
  byName.target = ByName{"nothing.xex", 1};
  auto absent = client.call(byName);
  REQUIRE_ERR(absent, ErrorCode::Io);
  CHECK(remoteFault(absent.error()).has_value());

  auto faulty = client.call(callAt(kFn, ReturnKind::Int, {Arg::i32(1)}));
  REQUIRE_ERR(faulty, ErrorCode::Io);
  CHECK(remoteFault(faulty.error()) == std::optional<RemoteFault>(RemoteFault::VersionMismatch));
  CHECK(faulty.error().message.ends_with("Version mismatch"));

  CHECK(client.isConnected());
  CHECK_EQ(got(client.callUInt32(callAt(kFn, ReturnKind::Int))), uint32_t{1});
  rig.checkCleanTraffic();
}

// --- A long session --------------------------------------------------------------------

JRPC_LINK_TEST(JrpcIntegration, AHundredCallsOnOneConnectionStayInStep) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &call) {
    const int64_t n = call.args.at(0).integer;
    switch (call.type) {
    case 1: return JrpcReturn::integer(static_cast<uint64_t>(n * 2 + 1));
    case 2: return JrpcReturn::string("call #" + std::to_string(n));
    case 5: return JrpcReturn::intArray({static_cast<int32_t>(n), static_cast<int32_t>(-n)});
    default: return JrpcReturn::real(static_cast<double>(n) / 4.0);
    }
  });
  auto client = rig.client();
  for (int n = 0; n < 100; ++n) {
    switch (n % 4) {
    case 0: {
      auto r = client.callUInt32(callAt(kFn, ReturnKind::Int, {Arg::i32(n)}));
      REQUIRE_OK(r);
      CHECK_EQ(*r, static_cast<uint32_t>(n * 2 + 1));
      break;
    }
    case 1: {
      auto r = client.callString(callAt(kFn, ReturnKind::String, {Arg::i32(n)}));
      REQUIRE_OK(r);
      CHECK_EQ(*r, "call #" + std::to_string(n));
      break;
    }
    case 2: {
      auto r = client.callInts(callAt(kFn, ReturnKind::IntArray, {Arg::i32(n)}, 2));
      REQUIRE_OK(r);
      CHECK_EQ(*r, (std::vector<int32_t>{n, -n}));
      break;
    }
    default: {
      auto r = client.callFloat(callAt(kFn, ReturnKind::Float, {Arg::i32(n)}));
      REQUIRE_OK(r);
      CHECK_EQ(*r, n / 4.0);
      break;
    }
    }
  }
  CHECK(client.isConnected());
  CHECK_EQ(rig.mock.calls().size(), size_t{100});
  CHECK_EQ(rig.mock.connectionsAccepted(), size_t{1});
  CHECK_EQ(rig.mock.commands().size(), size_t{100});
  for (const auto &call : rig.mock.calls()) CHECK_EQ(call.connection, size_t{0});
  rig.checkCleanTraffic();
  client.close();
  REQUIRE(rig.mock.waitForCommands(101));
  CHECK_EQ(rig.mock.commands().back().name, std::string("bye"));
}

JRPC_LINK_TEST(JrpcIntegration, MixedCallsAndOpcodesOnOneConnectionStayInStep) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &call) { return JrpcReturn::integer(static_cast<uint64_t>(call.args.at(0).integer)); });
  auto client = rig.client();
  for (uint32_t n = 0; n < 40; ++n) {
    CHECK_EQ(got(client.callUInt32(callAt(kFn, ReturnKind::Int, {Arg::u32(n)}))), n);
    CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
    CHECK_OK(client.notify("n=" + std::to_string(n), n));
    CHECK_EQ(got(client.currentTitleId()), uint32_t{0xFFFE07D1});
  }
  CHECK(client.isConnected());
  CHECK_EQ(rig.mock.notifications().size(), size_t{40});
  rig.checkCleanTraffic();
}

// --- System opcodes --------------------------------------------------------------------

JRPC_LINK_TEST(JrpcIntegration, EveryOpcodeReachesTheConsole) {
  Rig rig(link);
  rig.mock.registerFunction("xam.xex", 436, [](const JrpcCall &) { return JrpcReturn::integer(1); });
  ut::JrpcConsoleInfo info;
  info.kernelVersion = 17559;
  info.consoleType = "Corona";
  info.titleId = 0x4D5307E6;
  info.temperatures = {0x41, 0x42, 0x43, 0x44};
  rig.mock.setInfo(info);
  auto client = rig.client();

  auto address = client.resolveFunction("xam.xex", 436);
  REQUIRE_OK(address);
  CHECK_EQ(client.cpuKey()->hex(), std::string("A1B2C3D4E5F60718"));
  CHECK_OK(client.notify("Hello from the test", 7));
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
  CHECK_OK(client.setLeds(LedState::Green, LedState::Red, LedState::Off, LedState::Orange));
  CHECK_EQ(got(client.temperature(TemperatureSensor::Cpu)), uint32_t{0x41});
  CHECK_EQ(got(client.temperature(TemperatureSensor::Gpu)), uint32_t{0x42});
  CHECK_EQ(got(client.temperature(TemperatureSensor::Edram)), uint32_t{0x43});
  CHECK_EQ(got(client.temperature(TemperatureSensor::Mainboard)), uint32_t{0x44});
  CHECK_EQ(got(client.currentTitleId()), uint32_t{0x4D5307E6});
  CHECK(got(client.consoleType()) == ConsoleType::Corona);
  CHECK_OK(client.constantMemorySet(0x82000010, 0xDEADBEEF, 0x12345678u, 0x4D5307E6u));

  const auto notes = rig.mock.notifications();
  REQUIRE_EQ(notes.size(), size_t{1});
  CHECK_EQ(notes[0].text, std::string("Hello from the test"));
  CHECK_EQ(notes[0].type, uint32_t{7});
  const auto leds = rig.mock.ledWrites();
  REQUIRE_EQ(leds.size(), size_t{1});
  CHECK_EQ(leds[0].topLeft, 0x80);
  CHECK_EQ(leds[0].topRight, 0x08);
  CHECK_EQ(leds[0].bottomLeft, 0);
  CHECK_EQ(leds[0].bottomRight, 0x88);
  const auto tasks = rig.mock.memoryTasks();
  REQUIRE_EQ(tasks.size(), size_t{1});
  CHECK_EQ(tasks[0].address, uint32_t{0x82000010});
  CHECK_EQ(tasks[0].value, uint32_t{0xDEADBEEF});
  CHECK_EQ(tasks[0].useIf, uint32_t{1});
  CHECK_EQ(tasks[0].ifValue, uint32_t{0x12345678});
  CHECK_EQ(tasks[0].useTitle, uint32_t{1});
  CHECK_EQ(tasks[0].titleId, uint32_t{0x4D5307E6});

  // Then the console goes down.
  CHECK_OK(client.shutdown());
  CHECK(!client.isConnected());
  CHECK(rig.mock.waitForActiveConnections(0));
  const auto events = rig.mock.events();
  REQUIRE_EQ(events.size(), size_t{4});
  CHECK_EQ(events.back(), std::string("shutdown"));

  // Every opcode went out, in order (the explicit and the barrier's ConsoleType lines left out).
  std::vector<std::string> names;
  for (const auto &record : rig.mock.commands()) {
    if (record.name != "consoletype") names.push_back(record.name);
  }
  CHECK_EQ(names, (std::vector<std::string>{"resolve", "cpukey", "notify", "kernel", "leds", "temperature",
                                            "temperature", "temperature", "temperature", "titleid", "constmem",
                                            "shutdown"}));
  rig.checkCleanTraffic();
}

// D6: whatever the console does with the opcodes that are documented without a reply, the
// stream stays in step across a long mixed session.
JRPC_LINK_TEST(JrpcIntegration, SilentOpcodesNeverLeaveTheStreamOutOfStep) {
  for (const auto answer : {ut::JrpcSilentOpcodeAnswer::Silent, ut::JrpcSilentOpcodeAnswer::SOk, ut::JrpcSilentOpcodeAnswer::Zero}) {
    JrpcMockOptions o;
    o.silentOpcodes = answer;
    Rig rig(link, o);
    auto client = rig.client();
    for (int round = 0; round < 15; ++round) {
      CHECK_OK(client.notify("round", static_cast<uint32_t>(round)));
      CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
      CHECK_OK(client.setLeds(LedState::Off, LedState::Green, LedState::Off, LedState::Red));
      CHECK_EQ(got(client.currentTitleId()), uint32_t{0xFFFE07D1});
      CHECK_OK(client.constantMemorySet(0x82000010, static_cast<uint32_t>(round)));
      CHECK(got(client.consoleType()) == ConsoleType::Jasper);
    }
    CHECK(client.isConnected());
    CHECK_EQ(rig.mock.notifications().size(), size_t{15});
    CHECK_EQ(rig.mock.ledWrites().size(), size_t{15});
    CHECK_EQ(rig.mock.memoryTasks().size(), size_t{15});
    rig.checkCleanTraffic();
  }
}

JRPC_LINK_TEST(JrpcIntegration, WithoutTheBarrierASilentConsoleNeedsNoExtraCommand) {
  Rig rig(link);
  ClientOptions o = quickOptions();
  o.silentOpBarrier = false;
  auto client = rig.client(o);
  CHECK_OK(client.notify("quiet", 1));
  CHECK_OK(client.setLeds(LedState::Off, LedState::Off, LedState::Off, LedState::Off));
  CHECK_OK(client.constantMemorySet(0x82000010, 1));
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
  CHECK_EQ(rig.mock.commands().size(), size_t{4});
  CHECK(rig.recordsNamed("consoletype").empty());
  rig.checkCleanTraffic();
}

JRPC_LINK_TEST(JrpcIntegration, CpuKeyInEveryPaddingTheConsoleMightUse) {
  {
    JrpcMockOptions o;
    o.cpuKeyDigits = 16;
    Rig rig(link, o);
    auto client = rig.client();
    auto key = client.cpuKey();
    REQUIRE_OK(key);
    CHECK_EQ(key->bytes.size(), size_t{16});
    CHECK_EQ(key->hex(), std::string("00000000A1B2C3D400000000E5F60718"));
  }
  {
    // Unpadded, with a half that lost its leading zeros: reported, and the stream is fine.
    JrpcMockOptions o;
    o.cpuKeyPadded = false;
    Rig rig(link, o);
    ut::JrpcConsoleInfo info;
    info.cpuKeyHigh = 0x00A1B2C3;
    info.cpuKeyLow = 0x0000F607;
    rig.mock.setInfo(info);
    auto client = rig.client();
    CHECK_ERR(client.cpuKey(), ErrorCode::Protocol);
    CHECK(client.isConnected());
    CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
  }
}

JRPC_LINK_TEST(JrpcIntegration, ShutdownThenReconnectOnceTheConsoleIsBack) {
  Rig rig(link);
  auto client = rig.client();
  CHECK_OK(client.shutdown());
  CHECK(!client.isConnected());
  CHECK_EQ(client.lastDelivery()->command, std::string(opcodeName(Opcode::ShutDownConsole)));
  CHECK(client.lastDelivery()->delivery == Delivery::Sent);
  CHECK_ERR(client.kernelVersion(), ErrorCode::NotConnected);
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
  REQUIRE(rig.mock.waitForActiveConnections(0));
  REQUIRE_OK(client.reconnect());
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
  // No Bye was sent for the first connection; one will be for the second.
  client.close();
  REQUIRE(rig.mock.waitForCommands(3));
  size_t byes = 0;
  for (const auto &record : rig.mock.commands()) byes += record.name == "bye";
  CHECK_EQ(byes, size_t{1});
}

// --- The connection itself -------------------------------------------------------------

JRPC_LINK_TEST(JrpcIntegration, CloseSaysByeOnceAndTheDestructorDoesToo) {
  Rig rig(link);
  {
    auto client = rig.client();
    CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
    client.close();
    client.close();
    CHECK(!client.isConnected());
    CHECK_ERR(client.kernelVersion(), ErrorCode::NotConnected);
  }
  {
    auto client = rig.fresh();
    CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
  }
  REQUIRE(rig.mock.waitForActiveConnections(0));
  size_t byes = 0;
  for (const auto &record : rig.mock.commands()) byes += record.name == "bye";
  CHECK_EQ(byes, size_t{2});
}

JRPC_LINK_TEST(JrpcIntegration, TheTraceShowsWhatWentOverTheWire) {
  Rig rig(link);
  std::mutex mutex;
  std::vector<std::string> sent, received;
  ClientOptions o = quickOptions();
  o.trace = [&](TraceEvent event, std::string_view text, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK_EQ(bytes, uint64_t{0});
    (event == TraceEvent::Sent ? sent : received).push_back(std::string(text));
  };
  rig.mock.registerFunction(kFn, [](const JrpcCall &) { return JrpcReturn::integer(0x2A); });
  auto client = rig.client(o);
  CHECK_EQ(got(client.callUInt32(callAt(kFn, ReturnKind::Int, {Arg::i32(5)}))), uint32_t{42});
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
  std::lock_guard<std::mutex> lock(mutex);
  REQUIRE(received.size() >= 3);
  CHECK_EQ(received[0], std::string("JRPC2 connected"));
  CHECK_EQ(received[1], std::string("2A"));
  CHECK_EQ(received[2], std::string("17559"));
  const auto lines = rig.mock.commandLines();
  REQUIRE(sent.size() >= 2);
  REQUIRE(lines.size() >= 2);
  CHECK_EQ(sent[0], lines[0]);
  CHECK_EQ(sent[1], lines[1]);
}

JRPC_LINK_TEST(JrpcIntegration, ThePeerIsKnownOverTcpOnly) {
  Rig rig(link);
  auto client = rig.client();
  const auto peer = client.peer();
  if (link == Link::Tcp) {
    REQUIRE(peer.has_value());
    CHECK_EQ(peer->host, std::string("127.0.0.1"));
    CHECK_EQ(peer->port, rig.port());
  } else {
    CHECK(!peer.has_value());
  }
  CHECK(!client.describe().empty());
}

JRPC_LINK_TEST(JrpcIntegration, RawCommandsExchangeOneLineAndAnErrorLineIsAnAnswer) {
  Rig rig(link);
  auto client = rig.client();
  auto kernel = client.rawCommand(R"JR(consolefeatures ver=2 type=13 params="A\0\A\0\")JR");
  REQUIRE_OK(kernel);
  CHECK_EQ(kernel->line, std::string("17559"));
  CHECK(!kernel->isError);
  auto bad = client.rawCommand("consolefeatures ver=3 type=13 params=\"A\\0\\A\\0\\\"");
  REQUIRE_OK(bad);
  CHECK(bad->isError);
  CHECK(client.isConnected());
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
}
