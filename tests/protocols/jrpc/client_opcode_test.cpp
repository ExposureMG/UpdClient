#include "protocols/jrpc/client_fake.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/jrpc/client.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// The system opcode methods (resolveFunction ... constantMemorySet) against the scripted
// fake, with the command lines of docs/JRPC_PROTOCOL.md section 4.2 spelled out: what is
// sent, how each reply is read, what a bad reply does to the connection, the silent
// opcodes with and without the barrier (D6), the delivery record (D7), the CPU key (D8)
// and shutdown. The mock-driven set is in client_opcode_mock_test.cpp.

using namespace updclient;
using namespace updclient::jrpc;
using jt::FakeConsole;
using namespace std::chrono_literals;

namespace {

// The value of a result that is checked to have one; a default value if it has none, so
// that a failure is reported instead of aborting the run.
template <class T> T got(const Result<T> &result) {
  CHECK_OK(result);
  return result ? *result : T{};
}

constexpr std::string_view kResolve = R"JR(consolefeatures ver=2 type=9 params="A\0\A\2\2/7\78616D2E786578\1\436\")JR";
constexpr std::string_view kCpuKey = R"JR(consolefeatures ver=2 type=10 params="A\0\A\0\")JR";
constexpr std::string_view kShutdown = R"JR(consolefeatures ver=2 type=11 params="A\0\A\0\")JR";
constexpr std::string_view kNotify = R"JR(consolefeatures ver=2 type=12 params="A\0\A\2\2/5\48656C6C6F\1\0\")JR";
constexpr std::string_view kKernel = R"JR(consolefeatures ver=2 type=13 params="A\0\A\0\")JR";
constexpr std::string_view kLeds = R"JR(consolefeatures ver=2 type=14 params="A\0\A\4\1\128\1\8\1\0\1\136\")JR";
constexpr std::string_view kTemperatureGpu = R"JR(consolefeatures ver=2 type=15 params="A\0\A\1\1\1\")JR";
constexpr std::string_view kTitle = R"JR(consolefeatures ver=2 type=16 params="A\0\A\0\")JR";
// Also the barrier of D6.
constexpr std::string_view kConsoleType = R"JR(consolefeatures ver=2 type=17 params="A\0\A\0\")JR";
constexpr std::string_view kConstMem = R"JR(consolefeatures ver=2 type=18 params="A\82000010\A\5\1\-559038737\1\1\1\5\1\0\1\0\")JR";

std::string s(std::string_view v) { return std::string(v); }

// A console that answers every command with the same text (CR LF added).
std::shared_ptr<FakeConsole> answering(std::string_view reply) {
  auto console = FakeConsole::create();
  console->handle([text = std::string(reply)](FakeConsole &c, const std::string &) { c.line(text); });
  return console;
}

// A client on the console, with short timeouts unless the test says otherwise.
JrpcClient clientOn(const std::shared_ptr<FakeConsole> &console, ClientOptions options = jt::quickOptions()) {
  return jt::connected(console, std::move(options));
}

ClientOptions barrierOff() {
  ClientOptions options = jt::quickOptions();
  options.silentOpBarrier = false;
  return options;
}

} // namespace

// --- Opcodes that answer -------------------------------------------------------------

TEST(JrpcClientOpcodes, ResolveFunctionSendsTheModuleAndOrdinalAndReadsTheAddress) {
  auto console = FakeConsole::create();
  console->on(s(kResolve), "82010000\r\n");
  auto client = clientOn(console);
  auto address = client.resolveFunction("xam.xex", 436);
  REQUIRE_OK(address);
  CHECK_EQ(*address, uint32_t{0x82010000});
  CHECK_EQ(console->problems(), std::string());
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK_EQ(delivery->command, std::string("ResolveFunction"));
  CHECK(delivery->delivery == Delivery::Answered);
}

TEST(JrpcClientOpcodes, ResolveFunctionPassesAZeroAddressOnAndRefusesAnEmptyName) {
  auto console = FakeConsole::create();
  console->on(s(kResolve), "0\r\n");
  auto client = clientOn(console);
  auto zero = client.resolveFunction("xam.xex", 436);
  REQUIRE_OK(zero);
  CHECK_EQ(*zero, uint32_t{0});

  CHECK_ERR(client.resolveFunction("", 1), ErrorCode::InvalidArgument);
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
  CHECK_ERR(client.resolveFunction(std::string("a\0b", 3), 1), ErrorCode::InvalidArgument);
  CHECK_EQ(console->commands().size(), size_t{1});
  CHECK(client.isConnected());
}

TEST(JrpcClientOpcodes, ResolveFunctionOfAnUnknownExportIsARemoteFault) {
  auto console = FakeConsole::create();
  console->on(s(kResolve), "error=Could not resolve function address, params = x, 9\r\n").on(s(kKernel), "17559\r\n");
  auto client = clientOn(console);
  auto r = client.resolveFunction("xam.xex", 436);
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(remoteFault(r.error()) == std::optional<RemoteFault>(RemoteFault::CouldNotResolve));
  CHECK(client.isConnected());
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
}

TEST(JrpcClientOpcodes, ReadOnlyOpcodesReadTheirReplies) {
  auto console = FakeConsole::create();
  console->on(s(kKernel), "17559\r\n")
      .on(s(kTemperatureGpu), "2D\r\n")
      .on(s(kTitle), "FFFE07D1\r\n")
      .on(s(kConsoleType), "Corona\r\n");
  auto client = clientOn(console);
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
  CHECK_EQ(got(client.temperature(TemperatureSensor::Gpu)), uint32_t{0x2D});
  CHECK_EQ(got(client.currentTitleId()), uint32_t{0xFFFE07D1});
  CHECK(got(client.consoleType()) == ConsoleType::Corona);
  CHECK_EQ(console->problems(), std::string());
}

TEST(JrpcClientOpcodes, TheTemperatureIndexIsTheSensorsNumber) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &line) { c.line(line.find("1\\3\\\"") != std::string::npos ? "28" : "30"); });
  auto client = clientOn(console);
  CHECK_OK(client.temperature(TemperatureSensor::Cpu));
  CHECK_OK(client.temperature(TemperatureSensor::Gpu));
  CHECK_OK(client.temperature(TemperatureSensor::Edram));
  CHECK_EQ(got(client.temperature(TemperatureSensor::Mainboard)), uint32_t{0x28});
  const auto &commands = console->commands();
  REQUIRE_EQ(commands.size(), size_t{4});
  CHECK_EQ(commands[0], R"JR(consolefeatures ver=2 type=15 params="A\0\A\1\1\0\")JR");
  CHECK_EQ(commands[1], s(kTemperatureGpu));
  CHECK_EQ(commands[2], R"JR(consolefeatures ver=2 type=15 params="A\0\A\1\1\2\")JR");
  CHECK_EQ(commands[3], R"JR(consolefeatures ver=2 type=15 params="A\0\A\1\1\3\")JR");
}

TEST(JrpcClientOpcodes, ASensorOutsideTheTableIsRefusedBeforeAnythingIsSent) {
  auto console = FakeConsole::create();
  auto client = clientOn(console);
  CHECK_ERR(client.temperature(static_cast<TemperatureSensor>(4)), ErrorCode::InvalidArgument);
  CHECK_ERR(client.temperature(static_cast<TemperatureSensor>(0xFFFFFFFFu)), ErrorCode::InvalidArgument);
  CHECK_EQ(console->written(), size_t{0});
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
  CHECK_EQ(client.lastDelivery()->command, std::string("GetTemperature"));
  CHECK(client.isConnected());
}

TEST(JrpcClientOpcodes, EveryConsoleTypeNameIsRecognised) {
  struct Row {
    const char *name;
    ConsoleType type;
  };
  const Row rows[] = {{"Xenon", ConsoleType::Xenon},     {"Zephyr", ConsoleType::Zephyr},
                      {"Falcon", ConsoleType::Falcon},   {"Jasper", ConsoleType::Jasper},
                      {"Trinity", ConsoleType::Trinity}, {"Corona", ConsoleType::Corona},
                      {"Unknown", ConsoleType::Unknown}};
  for (const Row &row : rows) {
    auto client = clientOn(answering(row.name));
    auto type = client.consoleType();
    REQUIRE_OK(type);
    CHECK_MSG(*type == row.type, row.name);
    CHECK_MSG(consoleTypeName(*type) == std::string_view(row.name), row.name);
    CHECK(client.isConnected());
  }
}

TEST(JrpcClientOpcodes, AReplyOfTheWrongShapeClosesTheConnection) {
  struct Row {
    const char *name;
    const char *reply;
    std::function<bool(JrpcClient &)> run;
  };
  const std::vector<Row> rows = {
      {"resolve, text", "xyz", [](JrpcClient &c) { return c.resolveFunction("xam.xex", 1).has_value(); }},
      {"resolve, nine digits", "123456789", [](JrpcClient &c) { return c.resolveFunction("xam.xex", 1).has_value(); }},
      {"kernel, text", "ABC", [](JrpcClient &c) { return c.kernelVersion().has_value(); }},
      {"kernel, negative", "-5", [](JrpcClient &c) { return c.kernelVersion().has_value(); }},
      {"kernel, empty", "", [](JrpcClient &c) { return c.kernelVersion().has_value(); }},
      {"temperature, text", "hot", [](JrpcClient &c) { return c.temperature(TemperatureSensor::Cpu).has_value(); }},
      {"title id, sign", "-1", [](JrpcClient &c) { return c.currentTitleId().has_value(); }},
      {"console type, unknown name", "Slim", [](JrpcClient &c) { return c.consoleType().has_value(); }},
      {"console type, lower case", "jasper", [](JrpcClient &c) { return c.consoleType().has_value(); }},
      {"cpu key, text", "not a key!", [](JrpcClient &c) { return c.cpuKey().has_value(); }},
  };
  for (const Row &row : rows) {
    auto console = answering(row.reply);
    auto client = clientOn(console);
    CHECK_MSG(!row.run(client), row.name);
    CHECK_MSG(!client.isConnected(), row.name);
    CHECK_MSG(console->closed(), row.name);
    CHECK_MSG(console->byes() == 0, row.name);
  }
}

TEST(JrpcClientOpcodes, AnErrorLineIsARemoteFaultForEveryAnsweringOpcode) {
  const std::vector<std::pair<const char *, std::function<bool(JrpcClient &)>>> rows = {
      {"ResolveFunction", [](JrpcClient &c) { return c.resolveFunction("xam.xex", 1).has_value(); }},
      {"GetCpuKey", [](JrpcClient &c) { return c.cpuKey().has_value(); }},
      {"GetKernelVersion", [](JrpcClient &c) { return c.kernelVersion().has_value(); }},
      {"GetTemperature", [](JrpcClient &c) { return c.temperature(TemperatureSensor::Cpu).has_value(); }},
      {"GetCurrentTitleId", [](JrpcClient &c) { return c.currentTitleId().has_value(); }},
      {"ConsoleType", [](JrpcClient &c) { return c.consoleType().has_value(); }},
  };
  for (const auto &[name, run] : rows) {
    auto client = clientOn(answering("error=Version mismatch"));
    CHECK_MSG(!run(client), name);
    CHECK_MSG(client.isConnected(), name);
    auto delivery = client.lastDelivery();
    REQUIRE(delivery.has_value());
    CHECK_EQ(delivery->command, std::string(name));
    CHECK_MSG(delivery->delivery == Delivery::Answered, name);
  }
  auto client = clientOn(answering("error=Version mismatch"));
  auto r = client.currentTitleId();
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(remoteFault(r.error()) == std::optional<RemoteFault>(RemoteFault::VersionMismatch));
  CHECK(r.error().message.size() >= 16);
  CHECK(r.error().message.ends_with("Version mismatch"));
}

TEST(JrpcClientOpcodes, ADebugLineMeansJrpcIsNotInstalled) {
  auto client = clientOn(answering("DEBUG: unknown"));
  auto r = client.currentTitleId();
  REQUIRE_ERR(r, ErrorCode::Unsupported);
  CHECK(r.error().message.find("JRPC is not installed") != std::string::npos);
  CHECK(!client.isConnected());
}

TEST(JrpcClientOpcodes, ASilentConsoleEndsAnAnsweringOpcodeInATimeout) {
  auto console = FakeConsole::create();
  console->on(s(kKernel), "");
  ClientOptions options = jt::quickOptions();
  options.callTimeout = 100ms;
  auto client = clientOn(console, options);
  auto r = client.kernelVersion();
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(r.error().message.find("may have carried it out") != std::string::npos);
  CHECK(!client.isConnected());
  CHECK(client.lastDelivery()->delivery == Delivery::Sent);
}

// --- GetCPUKey (D8) ------------------------------------------------------------------

TEST(JrpcClientCpuKey, SixteenDigitsGiveAnEightByteKey) {
  auto console = FakeConsole::create();
  console->on(s(kCpuKey), "A1B2C3D4E5F60718\r\n");
  auto client = clientOn(console);
  auto key = client.cpuKey();
  REQUIRE_OK(key);
  CHECK(key->bytes == std::vector<uint8_t>({0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18}));
  CHECK_EQ(key->hex(), std::string("A1B2C3D4E5F60718"));
  CHECK_EQ(client.lastDelivery()->command, std::string("GetCpuKey"));
}

TEST(JrpcClientCpuKey, ThirtyTwoDigitsGiveASixteenByteKey) {
  auto console = FakeConsole::create();
  console->on(s(kCpuKey), "00000000A1B2C3D400000000E5F60718\r\n");
  auto client = clientOn(console);
  auto key = client.cpuKey();
  REQUIRE_OK(key);
  CHECK_EQ(key->bytes.size(), size_t{16});
  CHECK_EQ(key->hex(), std::string("00000000A1B2C3D400000000E5F60718"));
}

TEST(JrpcClientCpuKey, AnUnpaddedReplyIsReportedWithItsTextAndTheConnectionStays) {
  // A half with leading zeros loses them, so 15 digits cannot be split.
  auto console = FakeConsole::create();
  console->on(s(kCpuKey), "A1B2C3D45F60718\r\n").on(s(kKernel), "17559\r\n");
  auto client = clientOn(console);
  auto key = client.cpuKey();
  REQUIRE_ERR(key, ErrorCode::Protocol);
  CHECK(key.error().message.find("A1B2C3D45F60718") != std::string::npos);
  CHECK(client.isConnected());
  CHECK(client.lastDelivery()->delivery == Delivery::Answered);
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
}

TEST(JrpcClientCpuKey, OtherLengthsOfHexAreTheSameAmbiguity) {
  for (const char *reply : {"1", "0", "1234567", "123456789ABCDEF", "12345678901234567", "123456789012345678901234567890"}) {
    auto client = clientOn(answering(reply));
    auto key = client.cpuKey();
    CHECK_MSG(!key.has_value() && key.error().code == ErrorCode::Protocol, reply);
    CHECK_MSG(client.isConnected(), reply);
  }
}

TEST(JrpcClientCpuKey, ALineThatIsNotAKeyAtAllClosesTheConnection) {
  for (const char *reply : {"", "A1B2C3D4E5F6071G", "A1B2C3D4 E5F60718", "123456789012345678901234567890123", "hello"}) {
    auto console = answering(reply);
    auto client = clientOn(console);
    auto key = client.cpuKey();
    CHECK_MSG(!key.has_value() && key.error().code == ErrorCode::Protocol, reply);
    CHECK_MSG(!client.isConnected(), reply);
    CHECK_MSG(console->closed(), reply);
  }
}

// --- Opcodes that may not answer (D6) ------------------------------------------------

TEST(JrpcClientSilent, ASilentConsoleAnswersOnlyTheBarrier) {
  auto console = FakeConsole::create();
  console->on(s(kNotify), "").on(s(kConsoleType), "Jasper\r\n").on(s(kKernel), "17559\r\n");
  auto client = clientOn(console);
  CHECK_OK(client.notify("Hello", 0));
  // The barrier went out right behind the command, and its answer is gone.
  CHECK_EQ(console->commands().size(), size_t{2});
  CHECK_EQ(console->unread(), size_t{0});
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK_EQ(delivery->command, std::string("XNotify"));
  CHECK(delivery->delivery == Delivery::Answered);
  // The stream is in step.
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
  CHECK_EQ(console->problems(), std::string());
  CHECK(client.isConnected());
}

TEST(JrpcClientSilent, EachSilentOpcodeSendsItsGoldenLineThenTheBarrier) {
  auto console = FakeConsole::create();
  console->on(s(kNotify), "").on(s(kConsoleType), "Jasper\r\n")
      .on(s(kLeds), "").on(s(kConsoleType), "Jasper\r\n")
      .on(s(kConstMem), "").on(s(kConsoleType), "Jasper\r\n");
  auto client = clientOn(console);
  CHECK_OK(client.notify("Hello", 0));
  CHECK_OK(client.setLeds(LedState::Green, LedState::Red, LedState::Off, LedState::Orange));
  CHECK_OK(client.constantMemorySet(0x82000010, 0xDEADBEEF, 5u));
  CHECK_EQ(console->problems(), std::string());
  CHECK_EQ(console->commands().size(), size_t{6});
  CHECK_EQ(client.lastDelivery()->command, std::string("ConstantMemorySet"));
}

TEST(JrpcClientSilent, AnAnswerBeforeTheBarrierIsTheOpcodesOwnAndIsConsumed) {
  for (const char *answer : {"S_OK", "0", "80004005", "1A2B3C4D5E6F7081"}) {
    auto console = FakeConsole::create();
    console->handle([answer](FakeConsole &c, const std::string &line) {
      if (line.find("type=17") != std::string::npos) {
        c.line("Falcon");
      } else if (line.find("type=13") != std::string::npos) {
        c.line("17559");
      } else {
        c.line(answer);
      }
    });
    auto client = clientOn(console);
    CHECK_MSG(client.notify("Hello", 0).has_value(), answer);
    CHECK_MSG(client.setLeds(LedState::Off, LedState::Off, LedState::Off, LedState::Off).has_value(), answer);
    CHECK_MSG(client.constantMemorySet(0x82000010, 1).has_value(), answer);
    CHECK_MSG(console->unread() == 0, answer);
    // The next answer is the next command's own.
    auto version = client.kernelVersion();
    CHECK_MSG(version.has_value() && *version == 17559, answer);
    CHECK_MSG(client.isConnected(), answer);
  }
}

TEST(JrpcClientSilent, AnErrorLineFailsTheCallAfterTheBarrierAndKeepsTheStreamInStep) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &line) {
    if (line.find("type=17") != std::string::npos) {
      c.line("Jasper");
    } else if (line.find("type=13") != std::string::npos) {
      c.line("17559");
    } else {
      c.line("error=The limit of 40 has been reached");
    }
  });
  auto client = clientOn(console);
  auto r = client.constantMemorySet(0x82000010, 1);
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(remoteFault(r.error()) == std::optional<RemoteFault>(RemoteFault::Other));
  CHECK(r.error().message.ends_with("The limit of 40 has been reached"));
  CHECK(client.isConnected());
  CHECK(client.lastDelivery()->delivery == Delivery::Answered);
  CHECK_EQ(console->unread(), size_t{0});
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
  // The same for the other two.
  CHECK_ERR(client.notify("x", 1), ErrorCode::Io);
  CHECK_ERR(client.setLeds(LedState::Off, LedState::Off, LedState::Off, LedState::Off), ErrorCode::Io);
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
}

TEST(JrpcClientSilent, TwoLinesBeforeTheBarrierCloseTheConnection) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &line) {
    if (line.find("type=12") != std::string::npos) {
      c.line("S_OK").line("0");
    } else {
      c.line("Jasper");
    }
  });
  auto client = clientOn(console);
  auto r = client.notify("Hello", 0);
  REQUIRE_ERR(r, ErrorCode::Protocol);
  CHECK(!client.isConnected());
  CHECK(console->closed());
}

TEST(JrpcClientSilent, ALineThatIsNoAnswerClosesTheConnection) {
  for (const char *line : {"banana", "17559.5", "s_ok", "-1", "Slim"}) {
    auto console = answering(line);
    auto client = clientOn(console);
    auto r = client.setLeds(LedState::Off, LedState::Off, LedState::Off, LedState::Off);
    CHECK_MSG(!r.has_value() && r.error().code == ErrorCode::Protocol, line);
    CHECK_MSG(!client.isConnected(), line);
    CHECK_MSG(console->byes() == 0, line);
  }
}

TEST(JrpcClientSilent, ADebugLineMeansJrpcIsNotInstalled) {
  auto client = clientOn(answering("DEBUG"));
  CHECK_ERR(client.notify("Hello", 0), ErrorCode::Unsupported);
  CHECK(!client.isConnected());
}

TEST(JrpcClientSilent, AnUnansweredBarrierIsATimeoutThatSaysTheCommandMayHaveRun) {
  auto console = FakeConsole::create();
  console->on(s(kNotify), "").on(s(kConsoleType), "");
  ClientOptions options = jt::quickOptions();
  options.callTimeout = 100ms;
  auto client = clientOn(console, options);
  auto r = client.notify("Hello", 0);
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(r.error().message.find("may have carried it out") != std::string::npos);
  CHECK(!client.isConnected());
  CHECK(console->closed());
  CHECK(client.lastDelivery()->delivery == Delivery::Sent);
  CHECK_EQ(client.lastDelivery()->command, std::string("XNotify"));
}

TEST(JrpcClientSilent, AConnectionThatDiesAfterTheCommandIsSentButBeforeTheBarrierKeepsSent) {
  auto console = FakeConsole::create();
  console->dropAfterWritten(kNotify.size() + 2);
  console->on(s(kNotify), "");
  auto client = clientOn(console);
  auto r = client.notify("Hello", 0);
  REQUIRE_ERR(r, ErrorCode::Disconnected);
  CHECK(r.error().message.find("may have carried it out") != std::string::npos);
  CHECK(client.lastDelivery()->delivery == Delivery::Sent);
  CHECK(!client.isConnected());
}

TEST(JrpcClientSilent, TheBarrierIsNotAnotherCommandInTheDeliveryRecord) {
  auto console = FakeConsole::create();
  console->on(s(kNotify), "").on(s(kConsoleType), "Jasper\r\n");
  auto client = clientOn(console);
  CHECK_OK(client.notify("Hello", 0));
  CHECK_EQ(client.lastDelivery()->command, std::string("XNotify"));
}

TEST(JrpcClientSilent, WithoutTheBarrierTheCommandIsSentAndThatIsAll) {
  auto console = FakeConsole::create();
  console->on(s(kNotify), "").on(s(kLeds), "").on(s(kConstMem), "").on(s(kKernel), "17559\r\n");
  auto client = clientOn(console, barrierOff());
  CHECK_OK(client.notify("Hello", 0));
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK_EQ(delivery->command, std::string("XNotify"));
  CHECK(delivery->delivery == Delivery::Sent);
  CHECK_OK(client.setLeds(LedState::Green, LedState::Red, LedState::Off, LedState::Orange));
  CHECK_OK(client.constantMemorySet(0x82000010, 0xDEADBEEF, 5u));
  CHECK_EQ(console->commands().size(), size_t{3});
  CHECK_EQ(got(client.kernelVersion()), uint32_t{17559});
  CHECK_EQ(console->problems(), std::string());
}

TEST(JrpcClientSilent, WithoutTheBarrierAnAnswerIsLeftForTheNextCommand) {
  // The hazard the option documents: an answering console, a client that does not wait.
  auto console = FakeConsole::create();
  console->on(s(kNotify), "S_OK\r\n").on(s(kKernel), "17559\r\n");
  auto client = clientOn(console, barrierOff());
  CHECK_OK(client.notify("Hello", 0));
  auto version = client.kernelVersion();
  // `S_OK` is read as the kernel version; the connection is closed rather than
  // carrying on one reply behind.
  CHECK_ERR(version, ErrorCode::Protocol);
  CHECK(!client.isConnected());
}

TEST(JrpcClientSilent, TheBarrierCanBeSwitchedOnAndOffBetweenCommands) {
  auto console = FakeConsole::create();
  console->on(s(kNotify), "").on(s(kConsoleType), "Jasper\r\n").on(s(kNotify), "");
  auto client = clientOn(console);
  CHECK_OK(client.notify("Hello", 0));
  ClientOptions options = client.options();
  options.silentOpBarrier = false;
  client.setOptions(options);
  CHECK_OK(client.notify("Hello", 0));
  CHECK_EQ(console->commands().size(), size_t{3});
}

TEST(JrpcClientSilent, TheBarrierIsInTheTrace) {
  struct Event {
    TraceEvent event;
    std::string text;
  };
  std::vector<Event> events;
  ClientOptions options = jt::quickOptions();
  options.trace = [&](TraceEvent event, std::string_view text, uint64_t) { events.push_back({event, std::string(text)}); };
  auto console = FakeConsole::create();
  console->on(s(kNotify), "S_OK\r\n").on(s(kConsoleType), "Jasper\r\n");
  auto client = clientOn(console, options);
  events.clear();
  CHECK_OK(client.notify("Hello", 0));
  REQUIRE_EQ(events.size(), size_t{4});
  CHECK(events[0].event == TraceEvent::Sent);
  CHECK_EQ(events[0].text, s(kNotify));
  CHECK(events[1].event == TraceEvent::Sent);
  CHECK_EQ(events[1].text, s(kConsoleType));
  CHECK(events[2].event == TraceEvent::Received);
  CHECK_EQ(events[2].text, std::string("S_OK"));
  CHECK(events[3].event == TraceEvent::Received);
  CHECK_EQ(events[3].text, std::string("Jasper"));
}

TEST(JrpcClientSilent, ASecondCallDuringTheBarrierIsRefused) {
  JrpcClient *self = nullptr;
  std::vector<Result<uint32_t>> inner;
  ClientOptions options = jt::quickOptions();
  options.trace = [&](TraceEvent event, std::string_view, uint64_t) {
    if (event == TraceEvent::Received && self) inner.push_back(self->currentTitleId());
  };
  auto console = FakeConsole::create();
  console->on(s(kNotify), "").on(s(kConsoleType), "Jasper\r\n");
  auto client = clientOn(console, options);
  self = &client;
  auto r = client.notify("Hello", 0);
  self = nullptr;
  CHECK_OK(r);
  REQUIRE_EQ(inner.size(), size_t{1});
  CHECK_ERR(inner[0], ErrorCode::InvalidArgument);
  CHECK(client.isConnected());
}

// --- Arguments of the silent opcodes -------------------------------------------------

TEST(JrpcClientOpcodes, NotifyHexEncodesTheTextAndSendsTheIcon) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &line) {
    if (line.find("type=17") != std::string::npos) c.line("Jasper");
  });
  auto client = clientOn(console);
  CHECK_OK(client.notify("hi", 27));
  // U+00E9 in UTF-8 is C3 A9.
  CHECK_OK(client.notify("\xC3\xA9", 0xFFFFFFFFu));
  const auto &commands = console->commands();
  REQUIRE_EQ(commands.size(), size_t{4});
  CHECK_EQ(commands[0], R"JR(consolefeatures ver=2 type=12 params="A\0\A\2\2/2\6869\1\27\")JR");
  CHECK_EQ(commands[2], R"JR(consolefeatures ver=2 type=12 params="A\0\A\2\2/2\C3A9\1\-1\")JR");
}

TEST(JrpcClientOpcodes, NotifyRefusesWhatCannotBeSentWithoutSendingAnything) {
  auto console = FakeConsole::create();
  auto client = clientOn(console);
  CHECK_ERR(client.notify("", 0), ErrorCode::InvalidArgument);
  CHECK_ERR(client.notify(std::string("a\0b", 3), 0), ErrorCode::InvalidArgument);
  CHECK_ERR(client.notify("\xFF\xFE", 0), ErrorCode::InvalidArgument);
  CHECK_ERR(client.notify(std::string(5000, 'x'), 0), ErrorCode::LimitExceeded);
  CHECK_EQ(console->written(), size_t{0});
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
  CHECK_EQ(client.lastDelivery()->command, std::string("XNotify"));
  CHECK(client.isConnected());
}

TEST(JrpcClientOpcodes, SetLedsSendsTheRawValuesInOrder) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &line) {
    if (line.find("type=17") != std::string::npos) c.line("Jasper");
  });
  auto client = clientOn(console);
  CHECK_OK(client.setLeds(LedState::Green, LedState::Red, LedState::Off, LedState::Orange));
  CHECK_OK(client.setLeds(static_cast<LedState>(1), static_cast<LedState>(2), static_cast<LedState>(3), static_cast<LedState>(0x1234)));
  const auto &commands = console->commands();
  REQUIRE_EQ(commands.size(), size_t{4});
  CHECK_EQ(commands[0], s(kLeds));
  CHECK_EQ(commands[2], R"JR(consolefeatures ver=2 type=14 params="A\0\A\4\1\1\1\2\1\3\1\4660\")JR");
}

TEST(JrpcClientOpcodes, ConstantMemorySetSendsTheAddressAndTheGuards) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &line) {
    if (line.find("type=17") != std::string::npos) c.line("Jasper");
  });
  auto client = clientOn(console);
  CHECK_OK(client.constantMemorySet(0x82000010, 0xDEADBEEF, 5u));
  CHECK_OK(client.constantMemorySet(0x82000010, 1));
  CHECK_OK(client.constantMemorySet(0x82000010, 1, 0xFFFFFFFFu, 0x4D5307E6u));
  CHECK_OK(client.constantMemorySet(0x82000010, 0, std::nullopt, 0x4D5307E6u));
  CHECK_OK(client.constantMemorySet(0xFFFFFFFCu, 7, 0u));
  const auto &commands = console->commands();
  REQUIRE_EQ(commands.size(), size_t{10});
  CHECK_EQ(commands[0], s(kConstMem));
  CHECK_EQ(commands[2], R"JR(consolefeatures ver=2 type=18 params="A\82000010\A\5\1\1\1\0\1\0\1\0\1\0\")JR");
  CHECK_EQ(commands[4], R"JR(consolefeatures ver=2 type=18 params="A\82000010\A\5\1\1\1\1\1\-1\1\1\1\1297287142\")JR");
  CHECK_EQ(commands[6], R"JR(consolefeatures ver=2 type=18 params="A\82000010\A\5\1\0\1\0\1\0\1\1\1\1297287142\")JR");
  // A guard value of 0 is a guard all the same.
  CHECK_EQ(commands[8], R"JR(consolefeatures ver=2 type=18 params="A\FFFFFFFC\A\5\1\7\1\1\1\0\1\0\1\0\")JR");
}

TEST(JrpcClientOpcodes, ConstantMemorySetRefusesAddressZero) {
  auto console = FakeConsole::create();
  auto client = clientOn(console);
  CHECK_ERR(client.constantMemorySet(0, 1), ErrorCode::InvalidArgument);
  CHECK_EQ(console->written(), size_t{0});
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
  CHECK(client.isConnected());
}

TEST(JrpcClientOpcodes, TheLineLimitAppliesToEveryOpcode) {
  ClientOptions options = jt::quickOptions();
  options.maxCommandBytes = 60;
  auto console = FakeConsole::create();
  auto client = clientOn(console, options);
  // The notify line is 66 bytes.
  CHECK_ERR(client.notify("Hello", 0), ErrorCode::LimitExceeded);
  CHECK_ERR(client.setLeds(LedState::Off, LedState::Off, LedState::Off, LedState::Off), ErrorCode::LimitExceeded);
  CHECK_ERR(client.resolveFunction("xam.xex", 436), ErrorCode::LimitExceeded);
  CHECK_EQ(console->written(), size_t{0});
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
  CHECK(client.isConnected());
}

// --- ShutDownConsole -----------------------------------------------------------------

TEST(JrpcClientShutdown, SendsTheCommandThenClosesWithoutBye) {
  auto console = FakeConsole::create();
  console->on(s(kShutdown), "");
  auto client = clientOn(console);
  CHECK_OK(client.shutdown());
  CHECK(!client.isConnected());
  CHECK(console->closed());
  CHECK_EQ(console->byes(), 0);
  CHECK_EQ(console->wire(), s(kShutdown) + "\r\n");
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK_EQ(delivery->command, std::string("ShutDownConsole"));
  CHECK(delivery->delivery == Delivery::Sent);
  CHECK_EQ(console->problems(), std::string());
}

TEST(JrpcClientShutdown, IgnoresTheBarrierOption) {
  for (bool barrier : {true, false}) {
    auto console = FakeConsole::create();
    console->on(s(kShutdown), "");
    ClientOptions options = jt::quickOptions();
    options.silentOpBarrier = barrier;
    auto client = clientOn(console, options);
    CHECK_OK(client.shutdown());
    CHECK_EQ(console->wire(), s(kShutdown) + "\r\n");
    CHECK(!client.isConnected());
  }
}

TEST(JrpcClientShutdown, DoesNotWaitForAnAnswerAndLeavesItUnread) {
  auto console = FakeConsole::create();
  console->on(s(kShutdown), "S_OK\r\n");
  auto client = clientOn(console);
  CHECK_OK(client.shutdown());
  CHECK(!client.isConnected());
  CHECK_EQ(console->unread(), size_t{6});
}

TEST(JrpcClientShutdown, EveryCallAfterwardsIsNotConnectedUntilReconnect) {
  auto queue = std::make_shared<jt::ConsoleQueue>();
  auto first = FakeConsole::create();
  first->on(s(kShutdown), "");
  auto second = FakeConsole::create();
  second->on(s(kKernel), "17559\r\n");
  queue->push_back(first);
  queue->push_back(second);
  auto client = JrpcClient::open(jt::connectorFor(queue), jt::quickOptions());
  REQUIRE_OK(client);
  CHECK_OK(client->shutdown());
  CHECK_ERR(client->kernelVersion(), ErrorCode::NotConnected);
  CHECK_ERR(client->call(jt::intCall()), ErrorCode::NotConnected);
  CHECK(client->lastDelivery()->delivery == Delivery::NotSent);
  CHECK_OK(client->reconnect());
  CHECK_EQ(got(client->kernelVersion()), uint32_t{17559});
}

TEST(JrpcClientShutdown, AWriteThatDiesHalfWayIsPartlySentAndReported) {
  auto console = FakeConsole::create();
  console->dropAfterWritten(20);
  auto client = clientOn(console);
  auto r = client.shutdown();
  REQUIRE(!r.has_value());
  CHECK(r.error().message.find("may have carried it out") != std::string::npos);
  CHECK(client.lastDelivery()->delivery == Delivery::PartlySent);
  CHECK(!client.isConnected());
}

TEST(JrpcClientShutdown, ANotConnectedClientSendsNothing) {
  auto console = FakeConsole::create();
  auto client = clientOn(console);
  client.close();
  CHECK_ERR(client.shutdown(), ErrorCode::NotConnected);
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
  CHECK_EQ(client.lastDelivery()->command, std::string("ShutDownConsole"));
}

TEST(JrpcClientShutdown, StrayBytesFromTheConsoleStopItBeforeAnythingIsSent) {
  auto console = FakeConsole::create();
  console->send("JRPC2 connected\r\n");
  auto client = clientOn(console);
  // The banner arrived twice: the second copy is stray.
  CHECK_ERR(client.shutdown(), ErrorCode::Protocol);
  CHECK_EQ(console->written(), size_t{0});
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
}

// --- Every method on a client that cannot send ----------------------------------------

TEST(JrpcClientOpcodes, AMovedFromClientRefusesEveryOpcode) {
  auto console = FakeConsole::create();
  auto client = clientOn(console);
  JrpcClient other = std::move(client);
  CHECK_ERR(client.resolveFunction("m", 1), ErrorCode::NotConnected);
  CHECK_ERR(client.cpuKey(), ErrorCode::NotConnected);
  CHECK_ERR(client.shutdown(), ErrorCode::NotConnected);
  CHECK_ERR(client.notify("x", 0), ErrorCode::NotConnected);
  CHECK_ERR(client.kernelVersion(), ErrorCode::NotConnected);
  CHECK_ERR(client.setLeds(LedState::Off, LedState::Off, LedState::Off, LedState::Off), ErrorCode::NotConnected);
  CHECK_ERR(client.temperature(TemperatureSensor::Cpu), ErrorCode::NotConnected);
  CHECK_ERR(client.currentTitleId(), ErrorCode::NotConnected);
  CHECK_ERR(client.consoleType(), ErrorCode::NotConnected);
  CHECK_ERR(client.constantMemorySet(0x82000000, 0), ErrorCode::NotConnected);
}

TEST(JrpcClientOpcodes, AClosedClientRefusesEveryOpcodeAndSendsNothing) {
  auto console = FakeConsole::create();
  auto client = clientOn(console);
  client.close();
  const size_t written = console->written();
  CHECK_ERR(client.resolveFunction("m", 1), ErrorCode::NotConnected);
  CHECK_ERR(client.cpuKey(), ErrorCode::NotConnected);
  CHECK_ERR(client.notify("x", 0), ErrorCode::NotConnected);
  CHECK_ERR(client.kernelVersion(), ErrorCode::NotConnected);
  CHECK_ERR(client.setLeds(LedState::Off, LedState::Off, LedState::Off, LedState::Off), ErrorCode::NotConnected);
  CHECK_ERR(client.temperature(TemperatureSensor::Cpu), ErrorCode::NotConnected);
  CHECK_ERR(client.currentTitleId(), ErrorCode::NotConnected);
  CHECK_ERR(client.consoleType(), ErrorCode::NotConnected);
  CHECK_ERR(client.constantMemorySet(0x82000000, 0), ErrorCode::NotConnected);
  CHECK_EQ(console->written(), written);
  CHECK(client.lastDelivery()->delivery == Delivery::NotSent);
}

TEST(JrpcClientOpcodes, TheReadOnlyOpcodesAreTheOnesTheCliWillNotConfirm) {
  // D7: the library marks them, the CLI reads the mark.
  CHECK(isReadOnly(Opcode::ResolveFunction));
  CHECK(isReadOnly(Opcode::GetCpuKey));
  CHECK(isReadOnly(Opcode::GetKernelVersion));
  CHECK(isReadOnly(Opcode::GetTemperature));
  CHECK(isReadOnly(Opcode::GetCurrentTitleId));
  CHECK(isReadOnly(Opcode::ConsoleType));
  CHECK(!isReadOnly(Opcode::ShutDownConsole));
  CHECK(!isReadOnly(Opcode::XNotify));
  CHECK(!isReadOnly(Opcode::SetLeds));
  CHECK(!isReadOnly(Opcode::ConstantMemorySet));
}
