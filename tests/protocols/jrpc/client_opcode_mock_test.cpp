#include "protocols/jrpc/client_fake.hpp"
#include "support/jrpc_mock_server.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/jrpc/client.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The system opcode methods against the mock server, whose parser was written from the
// protocol document and shares nothing with the client: what the client builds has to be
// understood, and every reading the document leaves open (do opcodes 11, 12, 14 and 18
// answer, is the CPU key padded) has to leave the client in step with the console.

using namespace updclient;
using namespace updclient::jrpc;
using namespace std::chrono_literals;
using ut::JrpcCall;
using ut::JrpcConsoleInfo;
using ut::JrpcFault;
using ut::JrpcMockOptions;
using ut::JrpcMockServer;
using ut::JrpcReturn;
using ut::JrpcSilentOpcodeAnswer;

namespace {

// The value of a result that is checked to have one; a default value if it has none, so
// that a failure is reported instead of aborting the run.
template <class T> T got(const Result<T> &result) {
  CHECK_OK(result);
  return result ? *result : T{};
}

ClientOptions options() {
  ClientOptions o = jt::quickOptions();
  o.callTimeout = 3000ms;
  o.idleTimeout = 1000ms;
  return o;
}

ClientOptions withoutBarrier() {
  ClientOptions o = options();
  o.silentOpBarrier = false;
  return o;
}

Result<JrpcClient> connectTo(JrpcMockServer &mock, ClientOptions o = options()) {
  return JrpcClient::open(mock.connector(), std::move(o));
}

JrpcMockOptions answering(JrpcSilentOpcodeAnswer answer) {
  JrpcMockOptions o;
  o.silentOpcodes = answer;
  return o;
}

constexpr JrpcSilentOpcodeAnswer kAnswers[] = {JrpcSilentOpcodeAnswer::Silent, JrpcSilentOpcodeAnswer::SOk,
                                               JrpcSilentOpcodeAnswer::Zero};

} // namespace

TEST(JrpcOpcodeMock, TheReadingOpcodesReportTheConsole) {
  JrpcMockServer mock;
  JrpcConsoleInfo info;
  info.kernelVersion = 12345;
  info.consoleType = "Corona";
  info.titleId = 0x4D5307E6;
  info.temperatures = {0x41, 0x42, 0x43, 0x44};
  mock.setInfo(info);
  auto client = connectTo(mock);
  REQUIRE_OK(client);

  CHECK_EQ(got(client->kernelVersion()), uint32_t{12345});
  CHECK(got(client->consoleType()) == ConsoleType::Corona);
  CHECK_EQ(got(client->currentTitleId()), uint32_t{0x4D5307E6});
  CHECK_EQ(got(client->temperature(TemperatureSensor::Cpu)), uint32_t{0x41});
  CHECK_EQ(got(client->temperature(TemperatureSensor::Gpu)), uint32_t{0x42});
  CHECK_EQ(got(client->temperature(TemperatureSensor::Edram)), uint32_t{0x43});
  CHECK_EQ(got(client->temperature(TemperatureSensor::Mainboard)), uint32_t{0x44});
  CHECK(client->isConnected());

  const auto commands = mock.commands();
  REQUIRE_EQ(commands.size(), size_t{7});
  CHECK_EQ(commands[0].name, std::string("kernel"));
  CHECK_EQ(commands[1].name, std::string("consoletype"));
  CHECK_EQ(commands[2].name, std::string("titleid"));
  CHECK_EQ(commands[3].name, std::string("temperature"));
}

TEST(JrpcOpcodeMock, EveryConsoleTypeTheMockCanReportIsRead) {
  for (const char *name : {"Xenon", "Zephyr", "Falcon", "Jasper", "Trinity", "Corona", "Unknown"}) {
    JrpcMockServer mock;
    JrpcConsoleInfo info;
    info.consoleType = name;
    mock.setInfo(info);
    auto client = connectTo(mock);
    REQUIRE_OK(client);
    auto type = client->consoleType();
    REQUIRE_OK(type);
    CHECK_EQ(std::string(consoleTypeName(*type)), std::string(name));
  }
}

TEST(JrpcOpcodeMock, AnUnknownConsoleTypeNameClosesTheConnection) {
  JrpcMockServer mock;
  JrpcConsoleInfo info;
  info.consoleType = "Slim";
  mock.setInfo(info);
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  CHECK_ERR(client->consoleType(), ErrorCode::Protocol);
  CHECK(!client->isConnected());
}

TEST(JrpcOpcodeMock, ResolveFunctionFindsAnExportAndTheAddressCanBeCalled) {
  JrpcMockServer mock;
  const uint32_t address = mock.registerFunction("xam.xex", 436, [](const JrpcCall &) { return JrpcReturn::integer(99); });
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  auto resolved = client->resolveFunction("xam.xex", 436);
  REQUIRE_OK(resolved);
  CHECK_EQ(*resolved, address);
  // The mock compares module names without case.
  CHECK_EQ(got(client->resolveFunction("XAM.XEX", 436)), address);
  auto called = client->callUInt32(jt::callAt(*resolved, ReturnKind::Void));
  REQUIRE_OK(called);
  CHECK_EQ(*called, uint32_t{99});

  const auto commands = mock.commands();
  REQUIRE(commands.size() >= 1);
  CHECK_EQ(commands[0].name, std::string("resolve"));
}

TEST(JrpcOpcodeMock, ResolveFunctionOfAnUnknownExportIsAFaultAndTheConnectionStays) {
  JrpcMockServer mock;
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  auto r = client->resolveFunction("nothing.xex", 7);
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(remoteFault(r.error()) == std::optional<RemoteFault>(RemoteFault::CouldNotResolve));
  CHECK(client->isConnected());
  CHECK_OK(client->kernelVersion());
}

TEST(JrpcOpcodeMock, CpuKeyIsReadInEveryPaddingTheMockCanProduce) {
  {
    // The document's example: two halves of eight digits.
    JrpcMockServer mock;
    auto client = connectTo(mock);
    REQUIRE_OK(client);
    auto key = client->cpuKey();
    REQUIRE_OK(key);
    CHECK_EQ(key->hex(), std::string("A1B2C3D4E5F60718"));
  }
  {
    // The plan's reading: each half padded to 16 digits, 32 in all.
    JrpcMockOptions o;
    o.cpuKeyDigits = 16;
    JrpcMockServer mock(o);
    auto client = connectTo(mock);
    REQUIRE_OK(client);
    auto key = client->cpuKey();
    REQUIRE_OK(key);
    CHECK_EQ(key->bytes.size(), size_t{16});
    CHECK_EQ(key->hex(), std::string("00000000A1B2C3D400000000E5F60718"));
  }
  {
    // Unpadded, but both halves happen to have all their digits.
    JrpcMockOptions o;
    o.cpuKeyPadded = false;
    JrpcMockServer mock(o);
    auto client = connectTo(mock);
    REQUIRE_OK(client);
    CHECK_EQ(client->cpuKey()->hex(), std::string("A1B2C3D4E5F60718"));
  }
}

TEST(JrpcOpcodeMock, AnUnpaddedKeyWithShortHalvesIsReportedAndTheConnectionStays) {
  JrpcMockOptions o;
  o.cpuKeyPadded = false;
  JrpcMockServer mock(o);
  JrpcConsoleInfo info;
  info.cpuKeyHigh = 0x00A1B2C3;
  info.cpuKeyLow = 0x0000F607;
  mock.setInfo(info);
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  auto key = client->cpuKey();
  REQUIRE_ERR(key, ErrorCode::Protocol);
  CHECK(key.error().message.find("A1B2C3F607") != std::string::npos);
  CHECK(client->isConnected());
  CHECK_OK(client->kernelVersion());

  // The same console padded is fine.
  JrpcMockOptions padded;
  padded.cpuKeyPadded = true;
  mock.setOptions(padded);
  auto again = client->cpuKey();
  REQUIRE_OK(again);
  CHECK_EQ(again->hex(), std::string("00A1B2C30000F607"));
}

TEST(JrpcOpcodeMock, NotifyReachesTheConsoleWithItsTextAndIcon) {
  JrpcMockServer mock;
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  CHECK_OK(client->notify("Hello", 0));
  CHECK_OK(client->notify("caf\xC3\xA9 \xE2\x9C\x93 with spaces, \"quotes\" and \\ backslashes", 27));
  const auto notes = mock.notifications();
  REQUIRE_EQ(notes.size(), size_t{2});
  CHECK_EQ(notes[0].text, std::string("Hello"));
  CHECK_EQ(notes[0].type, uint32_t{0});
  CHECK_EQ(notes[1].text, std::string("caf\xC3\xA9 \xE2\x9C\x93 with spaces, \"quotes\" and \\ backslashes"));
  CHECK_EQ(notes[1].type, uint32_t{27});
  CHECK_EQ(mock.events().size(), size_t{2});
}

TEST(JrpcOpcodeMock, TheLongestNotifyThatFitsTheLineLimitArrivesIntact) {
  JrpcMockServer mock;
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  // The line is 56 + 2n + digits(n) bytes: 4066 characters make exactly 8191.
  const std::string longest(4066, 'x');
  CHECK_OK(client->notify(longest, 1));
  CHECK_ERR(client->notify(std::string(4067, 'x'), 1), ErrorCode::LimitExceeded);
  CHECK(client->isConnected());
  const auto notes = mock.notifications();
  REQUIRE_EQ(notes.size(), size_t{1});
  CHECK_EQ(notes[0].text.size(), size_t{4066});
  CHECK_EQ(notes[0].text, longest);
  const auto commands = mock.commands();
  REQUIRE(commands.size() >= 1);
  // 8191 bytes, CR and LF.
  CHECK_EQ(commands[0].length, size_t{8193});
}

TEST(JrpcOpcodeMock, SetLedsReachesTheConsoleInOrder) {
  JrpcMockServer mock;
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  CHECK_OK(client->setLeds(LedState::Green, LedState::Red, LedState::Off, LedState::Orange));
  CHECK_OK(client->setLeds(static_cast<LedState>(1), static_cast<LedState>(2), static_cast<LedState>(3), static_cast<LedState>(4)));
  const auto writes = mock.ledWrites();
  REQUIRE_EQ(writes.size(), size_t{2});
  CHECK_EQ(writes[0].topLeft, 0x80);
  CHECK_EQ(writes[0].topRight, 0x08);
  CHECK_EQ(writes[0].bottomLeft, 0x00);
  CHECK_EQ(writes[0].bottomRight, 0x88);
  CHECK_EQ(writes[1].bottomRight, 4);
}

TEST(JrpcOpcodeMock, ConstantMemorySetRegistersATaskWithItsGuards) {
  JrpcMockServer mock;
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  CHECK_OK(client->constantMemorySet(0x82000010, 0xDEADBEEF));
  CHECK_OK(client->constantMemorySet(0x82000020, 1, 5u));
  CHECK_OK(client->constantMemorySet(0x82000030, 2, 0xFFFFFFFFu, 0x4D5307E6u));
  CHECK_OK(client->constantMemorySet(0xFFFFFFFCu, 3, std::nullopt, 0u));
  const auto tasks = mock.memoryTasks();
  REQUIRE_EQ(tasks.size(), size_t{4});
  CHECK_EQ(tasks[0].address, uint32_t{0x82000010});
  CHECK_EQ(tasks[0].value, uint32_t{0xDEADBEEF});
  CHECK_EQ(tasks[0].useIf, uint32_t{0});
  CHECK_EQ(tasks[0].useTitle, uint32_t{0});
  CHECK_EQ(tasks[1].useIf, uint32_t{1});
  CHECK_EQ(tasks[1].ifValue, uint32_t{5});
  CHECK_EQ(tasks[2].ifValue, uint32_t{0xFFFFFFFF});
  CHECK_EQ(tasks[2].useTitle, uint32_t{1});
  CHECK_EQ(tasks[2].titleId, uint32_t{0x4D5307E6});
  CHECK_EQ(tasks[3].address, uint32_t{0xFFFFFFFC});
  CHECK_EQ(tasks[3].useIf, uint32_t{0});
  CHECK_EQ(tasks[3].useTitle, uint32_t{1});
  CHECK_EQ(tasks[3].titleId, uint32_t{0});
}

TEST(JrpcOpcodeMock, EventsArriveInTheOrderTheyWereAsked) {
  JrpcMockServer mock;
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  CHECK_OK(client->notify("one", 1));
  CHECK_OK(client->setLeds(LedState::Off, LedState::Off, LedState::Off, LedState::Off));
  CHECK_OK(client->constantMemorySet(0x82000000, 9));
  CHECK_OK(client->notify("two", 2));
  const auto events = mock.events();
  REQUIRE_EQ(events.size(), size_t{4});
  CHECK_EQ(events[0], std::string("notify 1 one"));
  CHECK_EQ(events[1], std::string("leds 0 0 0 0"));
  CHECK_EQ(events[2], std::string("constmem 82000000 00000009 00000000 00000000 00000000 00000000"));
  CHECK_EQ(events[3], std::string("notify 2 two"));
}

// --- D6: whatever the console does with the silent opcodes, the stream stays in step ----

TEST(JrpcOpcodeMock, WithTheBarrierTheClientStaysInStepWhateverTheSilentOpcodesAnswer) {
  for (const auto answer : kAnswers) {
    JrpcMockServer mock(answering(answer));
    auto client = connectTo(mock);
    REQUIRE_OK(client);
    for (int round = 0; round < 30; ++round) {
      CHECK_OK(client->notify("Hello", 0));
      auto version = client->kernelVersion();
      REQUIRE_OK(version);
      CHECK_EQ(*version, uint32_t{17559});
      CHECK_OK(client->setLeds(LedState::Green, LedState::Green, LedState::Off, LedState::Off));
      auto title = client->currentTitleId();
      REQUIRE_OK(title);
      CHECK_EQ(*title, uint32_t{0xFFFE07D1});
      CHECK_OK(client->constantMemorySet(0x82000010, static_cast<uint32_t>(round)));
      auto type = client->consoleType();
      REQUIRE_OK(type);
      CHECK(*type == ConsoleType::Jasper);
    }
    CHECK(client->isConnected());
    CHECK_EQ(mock.notifications().size(), size_t{30});
    CHECK_EQ(mock.ledWrites().size(), size_t{30});
    CHECK_EQ(mock.memoryTasks().size(), size_t{30});
  }
}

TEST(JrpcOpcodeMock, WithTheBarrierAnAnswerlessConsoleCostsTheMockOneExtraCommand) {
  JrpcMockServer mock;
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  CHECK_OK(client->notify("Hello", 0));
  const auto commands = mock.commands();
  REQUIRE_EQ(commands.size(), size_t{2});
  CHECK_EQ(commands[0].name, std::string("notify"));
  CHECK_EQ(commands[1].name, std::string("consoletype"));
}

TEST(JrpcOpcodeMock, WithoutTheBarrierASilentConsoleIsFine) {
  JrpcMockServer mock;
  auto client = connectTo(mock, withoutBarrier());
  REQUIRE_OK(client);
  for (int round = 0; round < 30; ++round) {
    CHECK_OK(client->notify("Hello", 0));
    CHECK_OK(client->setLeds(LedState::Off, LedState::Off, LedState::Off, LedState::Off));
    CHECK_OK(client->constantMemorySet(0x82000010, 1));
    auto version = client->kernelVersion();
    REQUIRE_OK(version);
    CHECK_EQ(*version, uint32_t{17559});
  }
  CHECK_EQ(mock.notifications().size(), size_t{30});
  // Nothing but the commands asked for went out.
  for (const auto &command : mock.commands()) CHECK(command.name != "consoletype");
}

TEST(JrpcOpcodeMock, WithoutTheBarrierAnAnsweringConsoleLeavesTheStreamOneReplyBehind) {
  {
    JrpcMockServer mock(answering(JrpcSilentOpcodeAnswer::SOk));
    auto client = connectTo(mock, withoutBarrier());
    REQUIRE_OK(client);
    CHECK_OK(client->notify("Hello", 0));
    // `S_OK` is what the kernel version call reads.
    CHECK_ERR(client->kernelVersion(), ErrorCode::Protocol);
    CHECK(!client->isConnected());
  }
  {
    // A stale `0` passes for a plausible value: the reason the barrier is the default.
    JrpcMockServer mock(answering(JrpcSilentOpcodeAnswer::Zero));
    auto client = connectTo(mock, withoutBarrier());
    REQUIRE_OK(client);
    CHECK_OK(client->notify("Hello", 0));
    auto title = client->currentTitleId();
    REQUIRE_OK(title);
    CHECK_EQ(*title, uint32_t{0});
  }
}

TEST(JrpcOpcodeMock, AnErrorLineFromASilentOpcodeIsAFaultAndTheStreamStaysInStep) {
  for (const auto answer : kAnswers) {
    for (const int type : {12, 14, 18}) {
      JrpcMockServer mock(answering(answer));
      mock.inject(JrpcFault::errorLine("The limit of 40 has been reached").onType(type));
      auto client = connectTo(mock);
      REQUIRE_OK(client);
      Result<void> r = [&]() -> Result<void> {
        if (type == 12) return client->notify("Hello", 0);
        if (type == 14) return client->setLeds(LedState::Off, LedState::Off, LedState::Off, LedState::Off);
        return client->constantMemorySet(0x82000010, 1);
      }();
      REQUIRE_ERR(r, ErrorCode::Io);
      CHECK(remoteFault(r.error()) == std::optional<RemoteFault>(RemoteFault::Other));
      CHECK(r.error().message.ends_with("The limit of 40 has been reached"));
      CHECK(client->isConnected());
      auto version = client->kernelVersion();
      REQUIRE_OK(version);
      CHECK_EQ(*version, uint32_t{17559});
    }
  }
}

TEST(JrpcOpcodeMock, BadArgumentsTheMockRefusesBecomeFaults) {
  // The mock checks the argument tags strictly; a console that does not understand the
  // line answers an error=, which the silent opcodes report like any other.
  JrpcMockServer mock;
  mock.inject(JrpcFault::errorLine("The paramaters were not found").onType(12).withoutCommand());
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  auto r = client->notify("Hello", 0);
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(remoteFault(r.error()) == std::optional<RemoteFault>(RemoteFault::ParametersNotFound));
  CHECK(mock.notifications().empty());
  CHECK_OK(client->notify("Hello", 0));
  CHECK_EQ(mock.notifications().size(), size_t{1});
}

TEST(JrpcOpcodeMock, ABarrierThatIsNeverAnsweredIsATimeoutAndClosesTheConnection) {
  JrpcMockServer mock;
  mock.inject(JrpcFault::silence().onType(17));
  ClientOptions o = options();
  o.callTimeout = 150ms;
  auto client = connectTo(mock, o);
  REQUIRE_OK(client);
  auto r = client->notify("Hello", 0);
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(r.error().message.find("may have carried it out") != std::string::npos);
  CHECK(!client->isConnected());
  CHECK(client->lastDelivery()->delivery == Delivery::Sent);
  // The console did carry it out.
  CHECK(mock.waitForCommands(2));
  CHECK_EQ(mock.notifications().size(), size_t{1});
}

TEST(JrpcOpcodeMock, ADroppedConnectionDuringASilentOpcodeIsDisconnected) {
  JrpcMockServer mock;
  mock.inject(JrpcFault::dropConnection().onType(14));
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  auto r = client->setLeds(LedState::Off, LedState::Off, LedState::Off, LedState::Off);
  REQUIRE_ERR(r, ErrorCode::Disconnected);
  CHECK(!client->isConnected());
  CHECK(client->lastDelivery()->delivery == Delivery::Sent);
}

TEST(JrpcOpcodeMock, AReplyOfTheWrongShapeToASilentOpcodeClosesTheConnection) {
  JrpcMockServer mock;
  mock.inject(JrpcFault::replyLine("banana").onType(18));
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  CHECK_ERR(client->constantMemorySet(0x82000010, 1), ErrorCode::Protocol);
  CHECK(!client->isConnected());
}

TEST(JrpcOpcodeMock, ADebugLineFromASilentOpcodeMeansJrpcIsNotInstalled) {
  JrpcMockServer mock;
  mock.inject(JrpcFault::debugLine().onType(12));
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  CHECK_ERR(client->notify("Hello", 0), ErrorCode::Unsupported);
  CHECK(!client->isConnected());
}

// --- Faults on the answering opcodes -------------------------------------------------

TEST(JrpcOpcodeMock, FaultsOnAnAnsweringOpcodeFollowTheCallRules) {
  struct Row {
    const char *name;
    JrpcFault fault;
    ErrorCode code;
    bool connected;
  };
  const std::vector<Row> rows = {
      {"error line", JrpcFault::errorLine("Version mismatch").onType(13), ErrorCode::Io, true},
      {"wrong shape", JrpcFault::replyLine("hello").onType(13), ErrorCode::Protocol, false},
      {"debug line", JrpcFault::debugLine().onType(13), ErrorCode::Unsupported, false},
      {"dropped", JrpcFault::dropConnection().onType(13), ErrorCode::Disconnected, false},
      {"silence", JrpcFault::silence().onType(13), ErrorCode::Timeout, false},
      {"over-long line", JrpcFault::oversizedLine().onType(13), ErrorCode::LimitExceeded, false},
  };
  for (const Row &row : rows) {
    JrpcMockServer mock;
    mock.inject(row.fault);
    ClientOptions o = options();
    o.callTimeout = 200ms;
    o.idleTimeout = 200ms;
    auto client = connectTo(mock, o);
    REQUIRE_OK(client);
    auto r = client->kernelVersion();
    CHECK_MSG(!r.has_value() && r.error().code == row.code, row.name);
    CHECK_MSG(client->isConnected() == row.connected, row.name);
  }
}

// --- ShutDownConsole -----------------------------------------------------------------

TEST(JrpcOpcodeMock, ShutdownIsRecordedAndClosesTheClient) {
  for (const auto answer : kAnswers) {
    JrpcMockServer mock(answering(answer));
    auto client = connectTo(mock);
    REQUIRE_OK(client);
    CHECK_OK(client->shutdown());
    CHECK(!client->isConnected());
    CHECK(mock.waitForActiveConnections(0));
    const auto events = mock.events();
    REQUIRE_EQ(events.size(), size_t{1});
    CHECK_EQ(events[0], std::string("shutdown"));
    CHECK_ERR(client->kernelVersion(), ErrorCode::NotConnected);
    // No Bye follows a shutdown, and nothing else was sent.
    for (const auto &command : mock.commands()) CHECK(command.name != "bye");
    CHECK_EQ(mock.commands().size(), size_t{1});
  }
}

TEST(JrpcOpcodeMock, ShutdownTakesDownTheOtherConnectionsOfTheConsoleToo) {
  JrpcMockServer mock;
  auto first = connectTo(mock);
  auto second = connectTo(mock);
  REQUIRE_OK(first);
  REQUIRE_OK(second);
  CHECK_OK(first->shutdown());
  CHECK(mock.waitForActiveConnections(0));
  auto r = second->kernelVersion();
  CHECK(!r.has_value());
  CHECK(!second->isConnected());
}

TEST(JrpcOpcodeMock, ShutdownCanLeaveTheOtherConnectionsAlone) {
  JrpcMockOptions o;
  o.shutdownDropsAllConnections = false;
  JrpcMockServer mock(o);
  auto first = connectTo(mock);
  auto second = connectTo(mock);
  REQUIRE_OK(first);
  REQUIRE_OK(second);
  CHECK_OK(first->shutdown());
  CHECK_OK(second->kernelVersion());
  CHECK(second->isConnected());
}

TEST(JrpcOpcodeMock, ReconnectAfterShutdownOpensANewConnection) {
  JrpcMockServer mock;
  auto client = connectTo(mock);
  REQUIRE_OK(client);
  CHECK_OK(client->shutdown());
  CHECK(!client->isConnected());
  // The console is down once it has dropped every connection; a client that comes back
  // sooner would be among them.
  REQUIRE(mock.waitForActiveConnections(0));
  REQUIRE_OK(client->reconnect());
  CHECK_EQ(got(client->kernelVersion()), uint32_t{17559});
}

// --- Several clients, and TCP --------------------------------------------------------

TEST(JrpcOpcodeMock, SeveralClientsMixSilentAndAnsweringOpcodesWithoutLosingStep) {
  for (const auto answer : kAnswers) {
    JrpcMockServer mock(answering(answer));
    constexpr int kThreads = 4;
    constexpr int kRounds = 25;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t] {
        auto client = connectTo(mock);
        if (!client) {
          ++failures;
          return;
        }
        for (int i = 0; i < kRounds; ++i) {
          if (!client->notify("t" + std::to_string(t), static_cast<uint32_t>(i))) ++failures;
          auto version = client->kernelVersion();
          if (!version || *version != 17559) ++failures;
          if (!client->setLeds(LedState::Off, LedState::Green, LedState::Off, LedState::Red)) ++failures;
          auto title = client->currentTitleId();
          if (!title || *title != 0xFFFE07D1u) ++failures;
        }
      });
    }
    for (auto &thread : threads) thread.join();
    CHECK_EQ(failures.load(), 0);
    CHECK_EQ(mock.notifications().size(), static_cast<size_t>(kThreads * kRounds));
  }
}

TEST(JrpcOpcodeMock, TheOpcodesWorkOverLoopbackTcp) {
  JrpcMockServer mock;
  mock.registerFunction("xam.xex", 436, [](const JrpcCall &) { return JrpcReturn::integer(1); });
  auto port = mock.listenTcp();
  if (!port) SKIP("loopback TCP refused: " + formatError(port.error()));
  net::Endpoint endpoint;
  endpoint.scheme = "jrpc";
  endpoint.host = "127.0.0.1";
  endpoint.port = *port;
  endpoint.timeout = 5000ms;
  auto client = JrpcClient::connect(endpoint, options());
  REQUIRE_OK(client);
  CHECK_EQ(got(client->kernelVersion()), uint32_t{17559});
  CHECK_EQ(client->cpuKey()->hex(), std::string("A1B2C3D4E5F60718"));
  CHECK(got(client->consoleType()) == ConsoleType::Jasper);
  CHECK_EQ(got(client->currentTitleId()), uint32_t{0xFFFE07D1});
  CHECK_EQ(got(client->temperature(TemperatureSensor::Gpu)), uint32_t{0x2D});
  CHECK_OK(client->resolveFunction("xam.xex", 436));
  CHECK_OK(client->notify("over tcp", 3));
  CHECK_OK(client->setLeds(LedState::Green, LedState::Off, LedState::Off, LedState::Off));
  CHECK_OK(client->constantMemorySet(0x82000010, 5, 1u));
  CHECK_EQ(mock.notifications().size(), size_t{1});
  CHECK_EQ(mock.ledWrites().size(), size_t{1});
  CHECK_EQ(mock.memoryTasks().size(), size_t{1});
  CHECK_OK(client->shutdown());
  CHECK(!client->isConnected());
  CHECK(mock.waitForActiveConnections(0));
}
