#include "protocols/jrpc/integration_support.hpp"

#include <atomic>
#include <climits>
#include <map>
#include <set>
#include <optional>
#include <thread>

// The mock server's misbehaviour against the client's obligations (D3, D4, D7): every
// failure ends in a clean error, never in a hang, a crash or bytes read as the answer to
// a later command, and a connection that is no longer trusted is closed. Also several
// connections at once, the console's limit of eight, and the calls that take too long or
// never return.

using namespace jit;
using namespace updclient::jrpc;

namespace {

constexpr uint32_t kFn = 0x82010000u;
constexpr uint32_t kFn2 = 0x82020000u;

JrpcReturn byType(const JrpcCall &call) {
  switch (call.type) {
  case 1: return JrpcReturn::integer(0x2A);
  case 2: return JrpcReturn::string("hello");
  case 3: return JrpcReturn::real(1.5);
  case 5: return JrpcReturn::intArray({1, -2, 3});
  case 6: return JrpcReturn::floatArray({1.5, 2.5, 3.5});
  case 7: return JrpcReturn::byteArray({1, 2, 3});
  case 8: return JrpcReturn::integer(0x1122334455667788ull);
  default: return JrpcReturn::integer(0);
  }
}

// One call of each kind, so that a fault can be aimed at any of them.
Result<void> callOfType(JrpcClient &client, int type) {
  switch (type) {
  case 1: return client.callUInt32(callAt(kFn, ReturnKind::Int)).transform([](uint32_t) {});
  case 2: return client.callString(callAt(kFn, ReturnKind::String)).transform([](const std::string &) {});
  case 3: return client.callFloat(callAt(kFn, ReturnKind::Float)).transform([](double) {});
  case 5: return client.callInts(callAt(kFn, ReturnKind::IntArray, {}, 3)).transform([](const std::vector<int32_t> &) {});
  case 6: return client.callFloats(callAt(kFn, ReturnKind::FloatArray, {}, 3)).transform([](const std::vector<double> &) {});
  case 7: return client.callBytes(callAt(kFn, ReturnKind::ByteArray, {}, 3)).transform([](const std::vector<uint8_t> &) {});
  default: return client.callUInt64(callAt(kFn, ReturnKind::Int64)).transform([](uint64_t) {});
  }
}

} // namespace

// --- Several connections ---------------------------------------------------------------

JRPC_LINK_TEST(JrpcFaultIntegration, TwoClientsInterleavedKeepTheirOwnConversation) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &call) {
    return JrpcReturn::integer(static_cast<uint64_t>(call.args.at(0).integer) * 10 + call.connection);
  });
  auto a = rig.client();
  auto b = rig.client();
  for (int n = 0; n < 20; ++n) {
    auto ra = a.callUInt32(callAt(kFn, ReturnKind::Int, {Arg::i32(n)}));
    auto rb = b.callUInt32(callAt(kFn, ReturnKind::Int, {Arg::i32(n + 100)}));
    REQUIRE_OK(ra);
    REQUIRE_OK(rb);
    // Connections are numbered in the order they arrived.
    CHECK_EQ(*ra, static_cast<uint32_t>(n * 10 + 0));
    CHECK_EQ(*rb, static_cast<uint32_t>((n + 100) * 10 + 1));
  }
  CHECK_EQ(rig.mock.activeConnections(), size_t{2});
  rig.checkCleanTraffic();
}

JRPC_LINK_TEST(JrpcFaultIntegration, TwoClientsOnTwoThreadsAtTheSameTime) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &call) {
    std::this_thread::sleep_for(1ms);
    return JrpcReturn::integer(static_cast<uint64_t>(call.args.at(0).integer) + 1);
  });
  rig.mock.registerFunction(kFn2, [](const JrpcCall &call) { return JrpcReturn::string("t" + std::to_string(call.args.at(0).integer)); });
  std::atomic<int> failures{0};
  constexpr int kRounds = 60;
  auto worker = [&](int id) {
    auto client = rig.open();
    if (!client) {
      ++failures;
      return;
    }
    for (int n = 0; n < kRounds; ++n) {
      const int value = id * 1000 + n;
      auto r = client->callUInt32(callAt(kFn, ReturnKind::Int, {Arg::i32(value)}));
      if (!r || *r != static_cast<uint32_t>(value + 1)) ++failures;
      auto s = client->callString(callAt(kFn2, ReturnKind::String, {Arg::i32(value)}));
      if (!s || *s != "t" + std::to_string(value)) ++failures;
    }
  };
  std::thread first(worker, 1), second(worker, 2);
  first.join();
  second.join();
  CHECK_EQ(failures.load(), 0);
  CHECK_EQ(rig.mock.calls().size(), static_cast<size_t>(4 * kRounds));
  // Each connection carried one client's values and nobody else's (which worker got which
  // connection depends on who connected first).
  std::map<size_t, std::set<int64_t>> workersOn;
  for (const auto &call : rig.mock.calls()) workersOn[call.connection].insert(call.args.at(0).integer / 1000);
  CHECK_EQ(workersOn.size(), size_t{2});
  for (const auto &entry : workersOn) CHECK_EQ(entry.second.size(), size_t{1});
  rig.checkCleanTraffic();
}

JRPC_LINK_TEST(JrpcFaultIntegration, ALongCallOnOneConnectionDoesNotHoldUpAnother) {
  Rig rig(link);
  auto entered = std::make_shared<Gate>();
  auto hold = std::make_shared<Gate>();
  Release release(hold);
  rig.mock.registerFunction(kFn, [entered, hold](const JrpcCall &) {
    entered->open();
    hold->wait(30s);
    return JrpcReturn::integer(77);
  });
  rig.mock.registerFunction(kFn2, [](const JrpcCall &) { return JrpcReturn::integer(5); });
  auto slow = rig.client();
  auto quick = rig.client();
  Result<uint32_t> slowResult = 0u;
  std::thread caller([&] { slowResult = slow.callUInt32(callAt(kFn, ReturnKind::Int)); });
  REQUIRE(entered->wait());
  for (int n = 0; n < 10; ++n) CHECK_EQ(got(quick.callUInt32(callAt(kFn2, ReturnKind::Int))), uint32_t{5});
  CHECK_EQ(got(quick.kernelVersion()), uint32_t{17559});
  hold->open();
  caller.join();
  REQUIRE_OK(slowResult);
  CHECK_EQ(*slowResult, uint32_t{77});
}

JRPC_LINK_TEST(JrpcFaultIntegration, TheNinthConnectionWaitsForASlot) {
  Rig rig(link);
  std::vector<JrpcClient> clients;
  for (int i = 0; i < 8; ++i) clients.push_back(rig.client());
  CHECK_EQ(rig.mock.activeConnections(), size_t{8});

  // No banner for the ninth: its open times out, and says why.
  ClientOptions brief = quickOptions();
  brief.bannerTimeout = 200ms;
  auto ninth = rig.open(brief);
  REQUIRE_ERR(ninth, ErrorCode::Timeout);
  CHECK(ninth.error().message.find("8 connections") != std::string::npos);

  // The eight keep working meanwhile.
  for (auto &client : clients) CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});

  // The connection that gave up still stands in line: like the real server, the mock
  // finds out only when it is its turn, so a ninth that comes later waits behind it.
  CHECK_EQ(rig.mock.waitingConnections(), size_t{1});
  std::optional<Result<JrpcClient>> late;
  std::thread waiter([&] { late.emplace(rig.open(quickOptions())); });
  CHECK(rig.mock.waitForWaitingConnections(2));
  CHECK_EQ(rig.mock.activeConnections(), size_t{8});
  // One slot frees: the abandoned connection takes it, sees nobody there and gives it
  // back, and the waiting client is served.
  clients[3].close();
  waiter.join();
  REQUIRE(late.has_value());
  REQUIRE_OK(*late);
  CHECK_EQ(got((*late)->kernelVersion()), uint32_t{17559});
  CHECK(rig.mock.waitForWaitingConnections(0));
}

JRPC_LINK_TEST(JrpcFaultIntegration, HeldConnectionsAreServedInTheOrderTheyArrived) {
  JrpcMockOptions o;
  o.connectionLimit = 1;
  Rig rig(link, o);
  auto first = rig.client();
  std::mutex mutex;
  std::vector<int> order;
  std::vector<std::thread> waiters;
  std::vector<std::optional<Result<JrpcClient>>> clients(3);
  for (int i = 0; i < 3; ++i) {
    waiters.emplace_back([&, i] {
      clients[static_cast<size_t>(i)].emplace(rig.open(quickOptions()));
      std::lock_guard<std::mutex> lock(mutex);
      order.push_back(i);
      if (clients[static_cast<size_t>(i)]->has_value()) (**clients[static_cast<size_t>(i)]).close();
    });
    // One at a time, so that the arrival order is known.
    CHECK(rig.mock.waitForWaitingConnections(static_cast<size_t>(i) + 1));
  }
  first.close();
  for (auto &t : waiters) t.join();
  CHECK_EQ(order, (std::vector<int>{0, 1, 2}));
  for (auto &c : clients) CHECK(c.has_value() && c->has_value());
}

// --- A reply that is cut short -----------------------------------------------------------

JRPC_LINK_TEST(JrpcFaultIntegration, ADropAtAnyPointOfAReplyIsDisconnectedAndReconnectRecovers) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  auto client = rig.client();
  size_t round = 0;
  // "hello", CR, LF: seven bytes, so a drop after 0 to 6 of them leaves an unfinished reply.
  for (size_t k = 0; k <= 6; ++k) {
    rig.mock.inject(JrpcFault::dropAfterBytes(k).onType(2));
    auto r = client.callString(callAt(kFn, ReturnKind::String));
    REQUIRE_ERR(r, ErrorCode::Disconnected);
    CHECK_MSG(!client.isConnected(), "after " + std::to_string(k) + " bytes");
    // The call was sent and its reply never completed; nothing more goes out on the dead connection.
    CHECK(client.lastDelivery()->delivery == Delivery::Sent);
    CHECK_ERR(client.callString(callAt(kFn, ReturnKind::String)), ErrorCode::NotConnected);
    REQUIRE(rig.mock.waitForActiveConnections(0));
    REQUIRE_OK(client.reconnect());
    ++round;
    CHECK_EQ(got(client.callString(callAt(kFn, ReturnKind::String))), std::string("hello"));
    CHECK_EQ(rig.mock.calls().back().connection, round);
  }
  CHECK(client.isConnected());
  CHECK_EQ(rig.mock.connectionsAccepted(), size_t{8});
}

JRPC_LINK_TEST(JrpcFaultIntegration, AFailedCallStillReportsHowFarItGotAfterTheReconnect) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  auto client = rig.client();
  rig.mock.inject(JrpcFault::dropAfterBytes(2).onType(1));
  auto r = client.callUInt32(callAt(kFn, ReturnKind::Int));
  REQUIRE_ERR(r, ErrorCode::Disconnected);
  CHECK(r.error().message.find("may have carried it out") != std::string::npos);
  REQUIRE(client.lastDelivery().has_value());
  CHECK(client.lastDelivery()->delivery == Delivery::Sent);
  CHECK_EQ(client.lastDelivery()->command, std::string("call"));
  REQUIRE(rig.mock.waitForActiveConnections(0));
  REQUIRE_OK(client.reconnect());
  // The record of the failed call survives the reconnect, so the caller can decide.
  CHECK(client.lastDelivery()->delivery == Delivery::Sent);
  CHECK_OK(client.callUInt32(callAt(kFn, ReturnKind::Int)));
  CHECK(client.lastDelivery()->delivery == Delivery::Answered);
}

JRPC_LINK_TEST(JrpcFaultIntegration, EveryReturnKindSurvivesBeingCutAtEveryByte) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  // Reply sizes with CR LF: "2A" 4, "hello" 7, "1.500000" 10, "1,-2,3;" 9, "1.500000,2.500000,3.500000;" 28,
  // "1,2,3;" 8, "1122334455667788" 18.
  struct Row {
    int type;
    size_t size;
  };
  for (const Row row : {Row{1, 4}, Row{2, 7}, Row{3, 10}, Row{5, 9}, Row{6, 28}, Row{7, 8}, Row{8, 18}}) {
    for (size_t k = 0; k < row.size; ++k) {
      rig.mock.inject(JrpcFault::dropAfterBytes(k).onType(row.type));
      auto client = rig.fresh();
      auto r = callOfType(client, row.type);
      CHECK_MSG(!r && r.error().code == ErrorCode::Disconnected,
                "type " + std::to_string(row.type) + " cut after " + std::to_string(k) + ": " +
                    (r ? "ok" : formatError(r.error())));
      CHECK(!client.isConnected());
    }
  }
}

JRPC_LINK_TEST(JrpcFaultIntegration, ACompleteReplyBeforeTheDropIsStillAnAnswer) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  rig.mock.inject(JrpcFault::dropAfterBytes(7).onType(2));
  auto client = rig.client();
  CHECK_EQ(got(client.callString(callAt(kFn, ReturnKind::String))), std::string("hello"));
  // The next call finds the connection gone.
  auto next = client.callString(callAt(kFn, ReturnKind::String));
  CHECK(!next);
  CHECK(!client.isConnected());
}

JRPC_LINK_TEST(JrpcFaultIntegration, StallsInsideAReplyAreTimeoutsAndCloseTheConnection) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  for (const size_t after : {size_t{1}, size_t{2}, size_t{3}}) {
    rig.mock.inject(JrpcFault::stall(after).onType(1));
    auto client = rig.fresh(impatient());
    const auto start = std::chrono::steady_clock::now();
    auto r = client.callUInt32(callAt(kFn, ReturnKind::Int));
    REQUIRE_ERR(r, ErrorCode::Timeout);
    // The idle timeout (300 ms) ends a started reply, not the call timeout.
    CHECK(msSince(start) >= 250);
    CHECK(msSince(start) < 3000);
    CHECK(!client.isConnected());
  }
}

JRPC_LINK_TEST(JrpcFaultIntegration, RepliesThatTrickleInByteByByteAreStillRead) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  rig.mock.inject(JrpcFault::trickleBytes().always());
  auto client = rig.client();
  for (const int type : {1, 2, 3, 5, 6, 7, 8}) CHECK_OK(callOfType(client, type));
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
  CHECK_EQ(got(client.consoleType()) == ConsoleType::Jasper, true);
  CHECK_OK(client.notify("trickle", 1));
  CHECK(client.isConnected());
}

// --- Replies that are not what was asked for ---------------------------------------------

JRPC_LINK_TEST(JrpcFaultIntegration, EachKindOfBadReplyEndsInTheRightErrorAndTheRightConnectionState) {
  struct Row {
    const char *name;
    JrpcFault fault;
    ErrorCode code;
    bool connected;
  };
  const std::vector<Row> rows = {
      {"error line", JrpcFault::errorLine("The paramaters were not found").onType(1), ErrorCode::Io, true},
      {"wrong shape", JrpcFault::replyLine("hello").onType(1), ErrorCode::Protocol, false},
      {"hex with a sign", JrpcFault::replyLine("-5").onType(1), ErrorCode::Protocol, false},
      {"empty line", JrpcFault::replyLine("").onType(1), ErrorCode::Protocol, false},
      {"debug line", JrpcFault::debugLine().onType(1), ErrorCode::Unsupported, false},
      {"over-long line", JrpcFault::oversizedLine().onType(1), ErrorCode::LimitExceeded, false},
      {"endless line", JrpcFault::endlessLine().onType(1), ErrorCode::LimitExceeded, false},
      {"dropped", JrpcFault::dropConnection().onType(1), ErrorCode::Disconnected, false},
      {"silence", JrpcFault::silence().onType(1), ErrorCode::Timeout, false},
  };
  for (const Row &row : rows) {
    Rig rig(link);
    rig.mock.registerFunction(kFn, byType);
    rig.mock.inject(row.fault);
    auto client = rig.client(impatient());
    auto r = client.callUInt32(callAt(kFn, ReturnKind::Int));
    CHECK_MSG(!r && r.error().code == row.code,
              std::string(row.name) + ": " + (r ? "ok" : formatError(r.error())));
    CHECK_MSG(client.isConnected() == row.connected, row.name);
    if (row.connected) {
      CHECK_MSG(remoteFault(r.error()).has_value(), row.name);
      CHECK_EQ(got(client.callUInt32(callAt(kFn, ReturnKind::Int))), uint32_t{0x2A});
    } else {
      CHECK_ERR(client.callUInt32(callAt(kFn, ReturnKind::Int)), ErrorCode::NotConnected);
      CHECK(client.lastDelivery().has_value());
    }
  }
}

JRPC_LINK_TEST(JrpcFaultIntegration, WrongShapeIsJudgedAgainstWhatWasAsked) {
  // The same reply is an answer to one kind and garbage to another; the kind decides.
  struct Row {
    const char *name;
    const char *line;
    ReturnKind kind;
    size_t arraySize;
    bool accepted;
  };
  const std::vector<Row> rows = {
      {"int as string", "2A", ReturnKind::String, 0, true},
      {"text as int", "banana", ReturnKind::Int, 0, false},
      {"float as int", "1.500000", ReturnKind::Int, 0, false},
      {"int as float", "2A", ReturnKind::Float, 0, false},
      {"array of three asked as two", "1,2,3;", ReturnKind::IntArray, 2, false},
      {"array of two asked as three", "1,2;", ReturnKind::IntArray, 3, false},
      {"array without semicolon", "1,2,3", ReturnKind::IntArray, 3, false},
      {"empty array", ";", ReturnKind::IntArray, 3, false},
      {"scalar as array", "2A", ReturnKind::ByteArray, 1, false},
      {"hex byte array", "A,FF,0;", ReturnKind::ByteArray, 3, true},
      {"trailing comma", "1,2,;", ReturnKind::IntArray, 3, false},
  };
  for (const Row &row : rows) {
    Rig rig(link);
    rig.mock.registerFunction(kFn, byType);
    rig.mock.inject(JrpcFault::replyLine(row.line).onAddress(kFn));
    auto client = rig.client();
    auto r = client.call(callAt(kFn, row.kind, {}, row.arraySize));
    CHECK_MSG(r.has_value() == row.accepted, row.name);
    CHECK_MSG(client.isConnected() == row.accepted, row.name);
    if (!row.accepted && !r) CHECK_MSG(r.error().code == ErrorCode::Protocol, row.name);
  }
}

JRPC_LINK_TEST(JrpcFaultIntegration, BytesThatBelongToNoCommandBreakTheNextCall) {
  // Behind the reply, in the same write so that they reach the client together with it (a
  // reply has no sequence number: bytes that arrive after the client has sent the next
  // command can only be taken for its answer): a second reply nobody asked for, bytes
  // without a line end, and bytes that never stop.
  JrpcFault endless;
  endless.endless = true;
  endless.type = 1;
  for (const auto &fault : {JrpcFault::reply("2A\r\n2B\r\n").onType(1), JrpcFault::reply("2A\r\njunk").onType(1), endless}) {
    Rig rig(link);
    rig.mock.registerFunction(kFn, byType);
    rig.mock.inject(fault);
    auto client = rig.client(impatient());
    CHECK_OK(client.callUInt32(callAt(kFn, ReturnKind::Int)));
    // Whatever followed is never taken for the answer to the next command.
    auto second = client.callUInt32(callAt(kFn, ReturnKind::Int));
    CHECK(!second);
    CHECK(!client.isConnected());
  }
}

JRPC_LINK_TEST(JrpcFaultIntegration, ADamagedReplyNeverHangsAndNeverLeavesAnUntrustedConnection) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  ClientOptions o = impatient();
  o.callTimeout = 300ms;
  o.idleTimeout = 150ms;
  size_t failed = 0;
  for (uint64_t seed = 1; seed <= 20; ++seed) {
    rig.mock.inject(JrpcFault::hostile(seed).onType(5));
    auto client = rig.fresh(o);
    const auto start = std::chrono::steady_clock::now();
    auto r = client.callInts(callAt(kFn, ReturnKind::IntArray, {}, 3));
    CHECK_MSG(msSince(start) < 5000, "seed " + std::to_string(seed) + " took long");
    if (r) {
      // A mangling that still reads as three integers: accepted, nothing more to say.
      CHECK_EQ(r->size(), size_t{3});
    } else {
      ++failed;
      // Only an error= line leaves the connection in use.
      if (r.error().code != ErrorCode::Io) CHECK_MSG(!client.isConnected(), "seed " + std::to_string(seed));
    }
  }
  CHECK(failed > 0);
}

// --- The banner --------------------------------------------------------------------------

JRPC_LINK_TEST(JrpcFaultIntegration, ABannerThatIsMissingWrongOrDroppedEndsTheOpen) {
  {
    JrpcMockOptions o;
    o.sendBanner = false;
    Rig rig(link, o);
    ClientOptions c = quickOptions();
    c.bannerTimeout = 200ms;
    CHECK_ERR(rig.open(c), ErrorCode::Timeout);
  }
  {
    JrpcMockOptions o;
    o.banner = "JRPC connected";
    Rig rig(link, o);
    CHECK_ERR(rig.open(), ErrorCode::Protocol);
  }
  {
    JrpcMockOptions o;
    o.banner = "201- connected";
    Rig rig(link, o);
    CHECK_ERR(rig.open(), ErrorCode::Protocol);
  }
  {
    Rig rig(link);
    rig.mock.inject(JrpcFault::debugLine().onGreeting());
    auto r = rig.open();
    REQUIRE_ERR(r, ErrorCode::Unsupported);
  }
  {
    Rig rig(link);
    rig.mock.inject(JrpcFault::dropAfterBytes(5).onGreeting());
    auto r = rig.open();
    CHECK(!r);
  }
  {
    Rig rig(link);
    rig.mock.inject(JrpcFault::trickleBytes().onGreeting());
    CHECK_OK(rig.open());
  }
}

// --- Timeouts and cancel -----------------------------------------------------------------

JRPC_LINK_TEST(JrpcFaultIntegration, ACallThatNeverAnswersEndsAtCallTimeout) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  rig.mock.inject(JrpcFault::silence().onType(1));
  auto client = rig.client(impatient());
  const auto start = std::chrono::steady_clock::now();
  auto r = client.callUInt32(callAt(kFn, ReturnKind::Int));
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(msSince(start) >= 500);
  CHECK(msSince(start) < 4000);
  CHECK(!client.isConnected());
  CHECK(client.lastDelivery()->delivery == Delivery::Sent);
}

JRPC_LINK_TEST(JrpcFaultIntegration, ACallThatOutlivesCallTimeoutClosesTheConnectionAndItsLateReplyIsLost) {
  Rig rig(link);
  auto entered = std::make_shared<Gate>();
  auto hold = std::make_shared<Gate>();
  Release release(hold);
  rig.mock.registerFunction(kFn, [entered, hold](const JrpcCall &) {
    entered->open();
    hold->wait(30s);
    return JrpcReturn::integer(99);
  });
  rig.mock.registerFunction(kFn2, [](const JrpcCall &) { return JrpcReturn::integer(5); });
  ClientOptions o = quickOptions();
  o.callTimeout = 300ms;
  auto client = rig.client(o);

  const auto start = std::chrono::steady_clock::now();
  auto r = client.callUInt32(callAt(kFn, ReturnKind::Int));
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(msSince(start) >= 250);
  CHECK(msSince(start) < 3000);
  CHECK(entered->isOpen());
  CHECK(r.error().message.find("may have carried it out") != std::string::npos);
  CHECK(!client.isConnected());
  CHECK(client.lastDelivery()->delivery == Delivery::Sent);
  CHECK_ERR(client.callUInt32(callAt(kFn2, ReturnKind::Int)), ErrorCode::NotConnected);

  // The function finishes and answers into a connection nobody reads any more.
  hold->open();
  REQUIRE(rig.mock.waitUntilIdle());
  REQUIRE(rig.mock.waitForActiveConnections(0));
  REQUIRE_OK(client.reconnect());
  // The new connection starts clean: the answer is its own, not the stale 99.
  CHECK_EQ(got(client.callUInt32(callAt(kFn2, ReturnKind::Int))), uint32_t{5});
  CHECK_EQ(got(client.callUInt32(callAt(kFn, ReturnKind::Int))), uint32_t{99});
  CHECK_EQ(rig.mock.callCount(kFn), size_t{2});
}

JRPC_LINK_TEST(JrpcFaultIntegration, ASlowCallIsNotCutShortByTheIdleTimeout) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, [](const JrpcCall &) {
    std::this_thread::sleep_for(500ms);
    return JrpcReturn::integer(7);
  });
  ClientOptions o = quickOptions();
  o.idleTimeout = 100ms;
  o.callTimeout = 5000ms;
  auto client = rig.client(o);
  CHECK_EQ(got(client.callUInt32(callAt(kFn, ReturnKind::Int))), uint32_t{7});
  CHECK(client.isConnected());

  // With no call timeout at all the wait for the first byte has no bound either.
  o.callTimeout = 0ms;
  client.setOptions(o);
  CHECK_EQ(got(client.callUInt32(callAt(kFn, ReturnKind::Int))), uint32_t{7});
  CHECK(client.isConnected());
}

JRPC_LINK_TEST(JrpcFaultIntegration, ADelayedReplyIsWaitedForUntilTheCallTimeout) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  rig.mock.inject(JrpcFault::delay(250ms).onType(1));
  auto client = rig.client(impatient());
  CHECK_EQ(got(client.callUInt32(callAt(kFn, ReturnKind::Int))), uint32_t{0x2A});
  rig.mock.inject(JrpcFault::delay(2s).onType(1));
  CHECK_ERR(client.callUInt32(callAt(kFn, ReturnKind::Int)), ErrorCode::Timeout);
  CHECK(!client.isConnected());
}

JRPC_LINK_TEST(JrpcFaultIntegration, CancelEndsACallThatNeverReturnsAndReconnectRecovers) {
  Rig rig(link);
  auto entered = std::make_shared<Gate>();
  auto hold = std::make_shared<Gate>();
  Release release(hold);
  rig.mock.registerFunction(kFn, [entered, hold](const JrpcCall &) {
    entered->open();
    hold->wait(30s);
    return JrpcReturn::integer(99);
  });
  rig.mock.registerFunction(kFn2, [](const JrpcCall &) { return JrpcReturn::integer(5); });
  // No call timeout and a long idle one: only cancel() can end the wait.
  ClientOptions o = quickOptions();
  o.callTimeout = 0ms;
  o.idleTimeout = 30s;
  auto client = rig.client(o);

  std::thread canceller([&] {
    entered->wait();
    std::this_thread::sleep_for(100ms);
    client.cancel();
  });
  const auto start = std::chrono::steady_clock::now();
  auto r = client.callUInt32(callAt(kFn, ReturnKind::Int));
  canceller.join();
  REQUIRE_ERR(r, ErrorCode::Cancelled);
  CHECK(msSince(start) < 5000);
  CHECK(!client.isConnected());
  CHECK(client.lastDelivery()->delivery == Delivery::Sent);
  // Until reconnect() nothing is sent.
  CHECK_ERR(client.callUInt32(callAt(kFn2, ReturnKind::Int)), ErrorCode::Cancelled);
  CHECK_EQ(rig.mock.calls().size(), size_t{1});

  // The console is still running the call, and other connections are served meanwhile,
  // the reconnected one included.
  REQUIRE_OK(client.reconnect());
  CHECK_EQ(got(client.callUInt32(callAt(kFn2, ReturnKind::Int))), uint32_t{5});
  auto other = rig.client();
  CHECK_EQ(got(other.callUInt32(callAt(kFn2, ReturnKind::Int))), uint32_t{5});
  CHECK(!hold->isOpen());
  hold->open();
  rig.mock.waitUntilIdle();
  CHECK_EQ(got(client.callUInt32(callAt(kFn2, ReturnKind::Int))), uint32_t{5});

  // No Bye went out on the cancelled connection.
  client.close();
  other.close();
  REQUIRE(rig.mock.waitForActiveConnections(0));
  size_t byes = 0;
  for (const auto &record : rig.mock.commands()) {
    if (record.name == "bye") {
      ++byes;
      CHECK(record.connection != 0);
    }
  }
  CHECK_EQ(byes, size_t{2});
}

JRPC_LINK_TEST(JrpcFaultIntegration, CancelEndsASilentConsoleWithoutAFunction) {
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  rig.mock.inject(JrpcFault::silence().onType(1));
  ClientOptions o = quickOptions();
  o.callTimeout = 0ms;
  o.idleTimeout = 30s;
  auto client = rig.client(o);
  std::thread canceller([&] {
    rig.mock.waitForCommands(1);
    std::this_thread::sleep_for(100ms);
    client.cancel();
  });
  auto r = client.callUInt32(callAt(kFn, ReturnKind::Int));
  canceller.join();
  REQUIRE_ERR(r, ErrorCode::Cancelled);
  REQUIRE(rig.mock.waitForActiveConnections(0));
  for (const auto &record : rig.mock.commands()) CHECK(record.name != "bye");
}

JRPC_LINK_TEST(JrpcFaultIntegration, CancelEndsAnOpcodeWaitingOnItsBarrier) {
  Rig rig(link);
  rig.mock.inject(JrpcFault::silence().onType(17));
  ClientOptions o = quickOptions();
  o.callTimeout = 0ms;
  o.idleTimeout = 30s;
  auto client = rig.client(o);
  std::thread canceller([&] {
    rig.mock.waitForCommands(2);
    std::this_thread::sleep_for(100ms);
    client.cancel();
  });
  auto r = client.notify("never confirmed", 1);
  canceller.join();
  REQUIRE_ERR(r, ErrorCode::Cancelled);
  CHECK(!client.isConnected());
  // The console did get the notification; the client says so.
  CHECK(client.lastDelivery()->delivery == Delivery::Sent);
  CHECK_EQ(rig.mock.notifications().size(), size_t{1});
}

JRPC_LINK_TEST(JrpcFaultIntegration, ReconnectAfterEveryKindOfFailureGivesAWorkingConnection) {
  struct Row {
    const char *name;
    JrpcFault fault;
  };
  const std::vector<Row> rows = {
      {"drop", JrpcFault::dropAfterBytes(1).onType(1)}, {"wrong shape", JrpcFault::replyLine("hello").onType(1)},
      {"debug", JrpcFault::debugLine().onType(1)},      {"silence", JrpcFault::silence().onType(1)},
      {"oversized", JrpcFault::oversizedLine().onType(1)},
  };
  Rig rig(link);
  rig.mock.registerFunction(kFn, byType);
  auto client = rig.client(impatient());
  for (const Row &row : rows) {
    rig.mock.clearFaults();
    rig.mock.inject(row.fault);
    CHECK_MSG(!client.callUInt32(callAt(kFn, ReturnKind::Int)), row.name);
    CHECK_MSG(!client.isConnected(), row.name);
    REQUIRE(rig.mock.waitForActiveConnections(0));
    // silence() stays armed until it is cleared, so the new connection would meet it too.
    rig.mock.clearFaults();
    REQUIRE_OK(client.reconnect());
    CHECK_MSG(client.isConnected(), row.name);
    CHECK_EQ(got(client.callUInt32(callAt(kFn, ReturnKind::Int))), uint32_t{0x2A});
  }
  rig.checkCleanTraffic();
}

JRPC_LINK_TEST(JrpcFaultIntegration, TheConsoleGoingAwayBetweenCallsIsDisconnected) {
  Rig rig(link);
  auto client = rig.client();
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
  rig.mock.dropAllConnections();
  REQUIRE(rig.mock.waitForActiveConnections(0));
  auto r = client.kernelVersion();
  REQUIRE(!r);
  CHECK(r.error().code == ErrorCode::Disconnected || r.error().code == ErrorCode::NotConnected);
  CHECK(!client.isConnected());
  REQUIRE_OK(client.reconnect());
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
}

JRPC_LINK_TEST(JrpcFaultIntegration, TheConsoleDisappearingForGoodMakesReconnectFail) {
  Rig rig(link);
  auto client = rig.client();
  rig.mock.stop();
  CHECK(!client.kernelVersion());
  CHECK(!client.isConnected());
  CHECK(!client.reconnect());
  CHECK(!client.isConnected());
}
