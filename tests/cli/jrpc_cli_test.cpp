#include "protocols/jrpc/integration_support.hpp"
#include "support/loopback_server.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

// The updclient executable against the JRPC mock over loopback TCP: the jrpc group, the
// JRPC halves of info and power shutdown, the refusals on a jrpc:// target, --trace,
// the JSON errors and Ctrl-C. The mock's parser is written from the protocol document,
// so what the CLI sends has to be understood by something that shares no code with it.

using namespace jit;

namespace {

struct Run {
  int exit = -1;
  std::string out;
  std::string err;
};

bool contains(const std::string &text, std::string_view part) { return text.find(part) != std::string::npos; }

#if defined(UPDCLIENT_CLI_PATH) && !defined(_WIN32)

// Runs the CLI with stdin from /dev/null, so destructive commands need --yes.
// `during` runs while the process is alive and may signal it.
Run runCli(const std::vector<std::string> &args, const std::function<void(pid_t)> &during = {}) {
  ut::TempDir io;
  const std::string out = io.file("out").string();
  const std::string err = io.file("err").string();
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  posix_spawn_file_actions_addopen(&actions, 2, err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  std::vector<std::string> all = {UPDCLIENT_CLI_PATH};
  all.insert(all.end(), args.begin(), args.end());
  std::vector<char *> argv;
  for (auto &a : all) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  Run run;
  const int spawned = posix_spawn(&pid, all[0].c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) {
    run.err = "posix_spawn failed: " + std::to_string(spawned);
    return run;
  }
  if (during) during(pid);
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  int status = 0;
  while (true) {
    const pid_t done = ::waitpid(pid, &status, WNOHANG);
    if (done == pid) break;
    if (std::chrono::steady_clock::now() > deadline) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &status, 0);
      run.err = "timed out\n";
      break;
    }
    std::this_thread::sleep_for(5ms);
  }
  run.exit = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  run.out += ut::textOf(ut::readFile(out).value_or(Bytes{}));
  run.err += ut::textOf(ut::readFile(err).value_or(Bytes{}));
  return run;
}

#else

Run runCli(const std::vector<std::string> &, const std::function<void(int)> & = {}) {
  ut::skip("the CLI is not built, or this platform has no posix_spawn");
}

#endif

std::string describe(const Run &run) {
  return "exit " + std::to_string(run.exit) + "\nstdout: " + run.out.substr(0, 600) + "\nstderr: " + run.err.substr(0, 800);
}

constexpr uint32_t kFunction = 0x82001000;

class CliRig {
public:
  CliRig() : rig(Link::Tcp) {}
  Rig rig;
  std::string target() const { return "jrpc://127.0.0.1:" + std::to_string(rig.port()); }
  Run runWithin(unsigned timeoutMs, std::vector<std::string> args, const std::function<void(int)> &during = {}) {
    args.insert(args.begin(), {"--target", target(), "--timeout-ms", std::to_string(timeoutMs)});
    return runCli(args, during);
  }
  Run run(std::vector<std::string> args, const std::function<void(int)> &during = {}) {
    return runWithin(3000, std::move(args), during);
  }
  // The registered function at kFunction returns this.
  void returns(JrpcReturn value) {
    rig.mock.registerFunction(kFunction, [value](const JrpcCall &) { return value; });
  }
};

} // namespace

TEST(JrpcCli, PingReadsTheBannerAndSendsNothingElse) {
  CliRig cli;
  auto text = cli.run({"jrpc", "ping"});
  CHECK_MSG(text.exit == 0, describe(text));
  CHECK_MSG(contains(text.out, "JRPC is installed at jrpc://127.0.0.1:"), describe(text));
  CHECK(contains(text.out, "JRPC2 connected"));
  auto json = cli.run({"--json", "jrpc", "ping"});
  CHECK_MSG(json.exit == 0 && contains(json.out, "\"banner\": \"JRPC2 connected\""), describe(json));
  CHECK_MSG(contains(json.out, "\"target\": \"jrpc://127.0.0.1:"), describe(json));
  CHECK(cli.rig.mock.waitUntilIdle());
  for (const auto &record : cli.rig.mock.commands()) CHECK_EQ(record.name, std::string("bye"));
  CHECK(cli.rig.mock.calls().empty());
}

TEST(JrpcCli, PingAnswersExitOneWhenNothingIsInstalled) {
  CliRig cli;
  cli.rig.mock.inject(JrpcFault::debugLine().onGreeting());
  auto run = cli.run({"--json", "jrpc", "ping"});
  CHECK_MSG(run.exit == 1 && contains(run.out, "\"code\": \"Unsupported\""), describe(run));
  CHECK_MSG(contains(run.err, "not installed"), describe(run));
}

TEST(JrpcCli, InfoAsksTheConsoleForEverything) {
  CliRig cli;
  auto text = cli.run({"jrpc", "info"});
  CHECK_MSG(text.exit == 0, describe(text));
  CHECK_MSG(contains(text.out, "JRPC CONSOLE INFO"), describe(text));
  CHECK(contains(text.out, "17559"));
  CHECK(contains(text.out, "Jasper"));
  CHECK(contains(text.out, "FFFE07D1"));
  CHECK(contains(text.out, "A1B2C3D4E5F60718"));
  CHECK_MSG(contains(text.out, "CPU Temp") && contains(text.out, "50 (0x32)"), describe(text));

  auto json = cli.run({"--json", "jrpc", "info"});
  CHECK_MSG(json.exit == 0, describe(json));
  CHECK_MSG(contains(json.out, "\"kernel_version\": 17559"), describe(json));
  CHECK(contains(json.out, "\"console_type\": \"Jasper\""));
  CHECK(contains(json.out, "\"title_id\": \"FFFE07D1\""));
  CHECK(contains(json.out, "\"cpu_key\": \"A1B2C3D4E5F60718\""));
  CHECK(contains(json.out, "\"cpu\": 50"));
  CHECK(contains(json.out, "\"mainboard\"") || contains(json.out, "\"board\": 40"));

  // The shared command takes the JRPC path on a jrpc:// target.
  auto shared = cli.run({"--json", "info"});
  CHECK_MSG(shared.exit == 0 && contains(shared.out, "\"kernel_version\": 17559"), describe(shared));
  CHECK(contains(shared.out, "\"temperatures\""));
  cli.rig.checkCleanTraffic();
}

TEST(JrpcCli, InfoLeavesAnUnansweredFieldEmpty) {
  CliRig cli;
  cli.rig.mock.inject(JrpcFault::errorLine("no sensor").onType(15).always());
  auto json = cli.run({"--json", "jrpc", "info"});
  CHECK_MSG(json.exit == 0, describe(json));
  CHECK(contains(json.out, "\"kernel_version\": 17559"));
  CHECK_MSG(contains(json.out, "\"cpu\": null"), describe(json));
  CHECK_MSG(contains(json.err, "no sensor") || contains(json.err, "temperature"), describe(json));
  auto text = cli.run({"jrpc", "info"});
  CHECK_MSG(text.exit == 0 && contains(text.out, "(not answered)") && contains(text.out, "Jasper"), describe(text));

  // A CPU key that cannot be split is a Protocol error for that field; the connection
  // stays and the other fields are still read.
  CliRig unpadded;
  JrpcMockOptions options = unpadded.rig.mock.options();
  options.cpuKeyPadded = false;
  unpadded.rig.mock.setOptions(options);
  auto info = unpadded.rig.mock.info();
  info.cpuKeyHigh = 0x1;
  unpadded.rig.mock.setInfo(info);
  auto partial = unpadded.run({"--json", "jrpc", "info"});
  CHECK_MSG(partial.exit == 0 && contains(partial.out, "\"cpu_key\": null"), describe(partial));
  CHECK_MSG(contains(partial.out, "\"title_id\": \"FFFE07D1\"") && contains(partial.out, "\"cpu\": 50"), describe(partial));
}

TEST(JrpcCli, InfoFailsWhenNothingAnswers) {
  CliRig cli;
  cli.rig.mock.inject(JrpcFault::errorLine("Version mismatch").always());
  auto run = cli.run({"--json", "jrpc", "info"});
  CHECK_MSG(run.exit == 1, describe(run));
  CHECK_MSG(contains(run.out, "\"remote_fault\": \"version_mismatch\""), describe(run));
  CHECK_MSG(!contains(run.out, "os_error"), describe(run));
}

TEST(JrpcCli, TheReadOnlyCommands) {
  CliRig cli;
  const uint32_t address = cli.rig.mock.registerFunction("xam.xex", 656, [](const JrpcCall &) { return JrpcReturn::integer(0); });

  auto cpukey = cli.run({"--json", "jrpc", "cpukey"});
  CHECK_MSG(cpukey.exit == 0 && contains(cpukey.out, "\"cpu_key\": \"A1B2C3D4E5F60718\""), describe(cpukey));
  CHECK(contains(cli.run({"jrpc", "cpukey"}).out, "CPU key: A1B2C3D4E5F60718"));
  auto kernel = cli.run({"--json", "jrpc", "kernel"});
  CHECK_MSG(kernel.exit == 0 && contains(kernel.out, "\"kernel_version\": 17559"), describe(kernel));
  CHECK(contains(cli.run({"jrpc", "kernel"}).out, "Kernel version: 17559"));
  auto type = cli.run({"--json", "jrpc", "console-type"});
  CHECK_MSG(type.exit == 0 && contains(type.out, "\"console_type\": \"Jasper\""), describe(type));
  auto title = cli.run({"--json", "jrpc", "title-id"});
  CHECK_MSG(title.exit == 0 && contains(title.out, "\"title_id\": \"FFFE07D1\""), describe(title));
  CHECK(contains(cli.run({"jrpc", "title-id"}).out, "Running title: FFFE07D1"));

  auto all = cli.run({"--json", "jrpc", "temp"});
  CHECK_MSG(all.exit == 0 && contains(all.out, "\"cpu\": 50") && contains(all.out, "\"gpu\": 45") &&
                contains(all.out, "\"edram\": 42") && contains(all.out, "\"board\": 40"),
            describe(all));
  auto gpu = cli.run({"jrpc", "temp", "GPU"});
  CHECK_MSG(gpu.exit == 0 && contains(gpu.out, "GPU:") && contains(gpu.out, "45 (0x2D)") && !contains(gpu.out, "CPU"),
            describe(gpu));
  auto bad = cli.run({"jrpc", "temp", "fan"});
  CHECK_MSG(bad.exit == 2 && contains(bad.err, "not a sensor"), describe(bad));

  auto resolve = cli.run({"--json", "jrpc", "resolve", "XAM.xex", "656"});
  CHECK_MSG(resolve.exit == 0, describe(resolve));
  char want[32];
  std::snprintf(want, sizeof want, "\"address\": \"%08X\"", address);
  CHECK_MSG(contains(resolve.out, want), describe(resolve));
  auto missing = cli.run({"--json", "jrpc", "resolve", "nothing.xex", "1"});
  CHECK_MSG(missing.exit == 1 && contains(missing.out, "\"remote_fault\": \"could_not_resolve\""), describe(missing));

  // Read-only commands never ask, and report no delivery.
  CHECK(!contains(missing.out, "command_delivery"));
  cli.rig.checkCleanTraffic();
}

TEST(JrpcCli, TheTargetMayBeABareHostOrAnIp) {
  CliRig cli;
  const std::string port = std::to_string(cli.rig.port());
  auto bare = runCli({"--target", "127.0.0.1", "--port", port, "--timeout-ms", "3000", "jrpc", "kernel"});
  CHECK_MSG(bare.exit == 0 && contains(bare.out, "17559"), describe(bare));
  auto ip = runCli({"--ip", "127.0.0.1", "--port", port, "--timeout-ms", "3000", "jrpc", "kernel"});
  CHECK_MSG(ip.exit == 0 && contains(ip.out, "17559"), describe(ip));
  auto conflicting = runCli({"--target", "jrpc://127.0.0.1:" + port, "--port", "1", "jrpc", "kernel"});
  CHECK_MSG(conflicting.exit == 2 && contains(conflicting.err, "conflicting ports"), describe(conflicting));
}

TEST(JrpcCli, ATargetIsRequiredAndNeverDiscovered) {
  auto none = runCli({"jrpc", "kernel"});
  CHECK_MSG(none.exit == 2 && contains(none.err, "never auto-discovered"), describe(none));
  auto xbdm = runCli({"--target", "xbdm://127.0.0.1", "jrpc", "kernel"});
  CHECK_MSG(xbdm.exit == 2 && contains(xbdm.err, "not to an xbdm:// target"), describe(xbdm));
  auto ping = runCli({"--json", "jrpc", "ping"});
  CHECK_MSG(ping.exit == 2 && contains(ping.out, "\"code\": \"Usage\""), describe(ping));
}

TEST(JrpcCli, ACallNeedsConfirmation) {
  CliRig cli;
  cli.returns(JrpcReturn::integer(42));
  auto refused = cli.run({"jrpc", "call", "0x82001000", "--returns", "int"});
  CHECK_MSG(refused.exit == 2 && contains(refused.err, "--yes"), describe(refused));
  CHECK(cli.rig.mock.calls().empty());
  CHECK_EQ(cli.rig.mock.callCount(kFunction), size_t{0});

  auto run = cli.run({"--yes", "jrpc", "call", "0x82001000", "--returns", "int"});
  CHECK_MSG(run.exit == 0, describe(run));
  CHECK_MSG(contains(run.out, "value : 42 (0x0000002A)") && contains(run.out, "reply : 2A"), describe(run));
  CHECK_EQ(cli.rig.mock.callCount(kFunction), size_t{1});
  cli.rig.checkCleanTraffic();
}

TEST(JrpcCli, CallSendsTheArgumentsAsGiven) {
  CliRig cli;
  cli.returns(JrpcReturn::integer(7));
  auto run = cli.run({"--yes", "--json", "jrpc", "call", "0x82001000", "--returns", "int", "--arg", "i32:-5", "--arg",
                      "u32:0xFFFFFFFF", "--arg", "bool:true", "--arg", "byte:255", "--arg", "i64:-2", "--arg", "u64:0x100000000",
                      "--arg", "str:a:b", "--arg", "bytes:DEADBEEF", "--arg", "ints:1,-2,3", "--arg", "f32:1.5", "--arg",
                      "f64:2.25"});
  CHECK_MSG(run.exit == 0, describe(run));
  CHECK(contains(run.out, "\"type\": \"int\""));
  CHECK(contains(run.out, "\"value\": 7"));
  CHECK(contains(run.out, "\"line\": \"7\""));
  const auto calls = cli.rig.mock.calls();
  REQUIRE_EQ(calls.size(), size_t{1});
  const auto &call = calls[0];
  CHECK_EQ(call.type, 1);
  CHECK(!call.system);
  CHECK_EQ(call.address, kFunction);
  REQUIRE_EQ(call.args.size(), size_t{11});
  CHECK_EQ(call.args[0].integer, int64_t{-5});
  CHECK_EQ(call.args[1].integer, int64_t{-1});  // a uint32 above INT32_MAX goes out as the equal negative int
  CHECK_EQ(call.args[2].integer, int64_t{1});
  CHECK_EQ(call.args[3].integer, int64_t{255});
  CHECK_EQ(call.args[4].tag, '8');
  CHECK_EQ(call.args[4].integer, int64_t{-2});
  CHECK_EQ(call.args[5].integer, int64_t{0x100000000});
  CHECK_EQ(call.args[6].tag, '2');
  CHECK_EQ(call.args[6].text(), std::string("a:b"));
  CHECK_EQ(call.args[7].tag, '7');
  CHECK_EQ(call.args[7].data, (Bytes{0xDE, 0xAD, 0xBE, 0xEF}));
  CHECK_EQ(call.args[8].data, (Bytes{0, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0xFE, 0, 0, 0, 3}));
  CHECK_EQ(call.args[9].tag, '3');
  CHECK_EQ(call.args[9].real, 1.5);
  CHECK_EQ(call.args[10].real, 2.25);
}

TEST(JrpcCli, CallDecodesEveryReturnKind) {
  CliRig cli;
  struct Case {
    const char *kind;
    const char *count;
    JrpcReturn reply;
    const char *json;
    const char *text;
  };
  const std::vector<Case> cases = {
      {"void", nullptr, JrpcReturn::integer(0), "\"value\": null", "reply : 0"},
      {"int", nullptr, JrpcReturn::integer(0xFFFFFFFFu), "\"value\": 4294967295", "signed -1"},
      {"str", nullptr, JrpcReturn::string("hello world"), "\"value\": \"hello world\"", "value : hello world"},
      {"float", nullptr, JrpcReturn::real(1.5), "\"value\": 1.5", "value : 1.5"},
      {"byte", nullptr, JrpcReturn::integer(0x1AB), "\"value\": 171", "(0xAB)"},
      {"int64", nullptr, JrpcReturn::integer(0x123456789ull), "\"value\": 4886718345", "0x0000000123456789"},
      {"ints", "3", JrpcReturn::intArray({1, -2, 3}), "\"value\": [\n    1,\n    -2,\n    3\n  ]", "value : 1, -2, 3"},
      {"floats", "2", JrpcReturn::floatArray({0.5, 2.0}), "\"value\": [\n    0.5,\n    2.0\n  ]", "value : 0.5, 2"},
      {"bytes", "4", JrpcReturn::byteArray({0xDE, 0xAD, 0xBE, 0xEF}), "\"value\": \"DEADBEEF\"", "value : DEADBEEF"},
  };
  for (const auto &c : cases) {
    cli.returns(c.reply);
    std::vector<std::string> args = {"--yes", "--json", "jrpc", "call", "0x82001000", "--returns", c.kind};
    if (c.count) args.insert(args.end(), {"--count", c.count});
    auto json = cli.run(args);
    CHECK_MSG(json.exit == 0 && contains(json.out, c.json) && contains(json.out, std::string("\"type\": \"") + c.kind + "\""),
              std::string(c.kind) + "\n" + describe(json));
    args.erase(args.begin() + 1);
    auto text = cli.run(args);
    CHECK_MSG(text.exit == 0 && contains(text.out, c.text), std::string(c.kind) + "\n" + describe(text));
  }
  cli.rig.checkCleanTraffic();
}

TEST(JrpcCli, CallByExportAndInTheSystemContext) {
  CliRig cli;
  const uint32_t address = cli.rig.mock.registerFunction("xboxkrnl.exe", 25, [](const JrpcCall &) { return JrpcReturn::integer(9); });
  auto run = cli.run({"--yes", "jrpc", "call", "xboxkrnl.exe!25", "--system", "--returns", "int"});
  CHECK_MSG(run.exit == 0 && contains(run.out, "value : 9"), describe(run));
  const auto calls = cli.rig.mock.calls();
  REQUIRE_EQ(calls.size(), size_t{1});
  CHECK(calls[0].system);
  CHECK_EQ(calls[0].module.value_or(""), std::string("xboxkrnl.exe"));
  CHECK_EQ(calls[0].ordinal, uint32_t{25});
  CHECK_EQ(calls[0].address, address);
}

TEST(JrpcCli, ACallThatCannotBeBuiltIsAUsageErrorBeforeAnythingIsSent) {
  CliRig cli;
  cli.returns(JrpcReturn::integer(1));
  struct Case {
    std::vector<std::string> args;
    const char *why;
  };
  const std::vector<Case> cases = {
      {{"jrpc", "call", "banana"}, "neither an address"},
      {{"jrpc", "call", "!5"}, "module!ordinal"},
      {{"jrpc", "call", "xam.xex!x"}, "module!ordinal"},
      {{"jrpc", "call", "0x82001000", "--returns", "wide"}, "--returns"},
      {{"jrpc", "call", "0x82001000", "--returns", "ints"}, "--count"},
      {{"jrpc", "call", "0x82001000", "--count", "2"}, "--count only applies"},
      {{"jrpc", "call", "0x82001000", "--returns", "ints", "--count", "9"}, "--count must be"},
      {{"jrpc", "call", "0x82001000", "--returns", "ints", "--count", "0"}, "--count must be"},
      {{"jrpc", "call", "0x82001000", "--arg", "5"}, "type:value"},
      {{"jrpc", "call", "0x82001000", "--arg", "i32:abc"}, "--arg"},
      {{"jrpc", "call", "0x82001000", "--arg", "i32:2147483648"}, "--arg"},
      {{"jrpc", "call", "0x82001000", "--arg", "u32:-1"}, "--arg"},
      {{"jrpc", "call", "0x82001000", "--arg", "byte:256"}, "--arg"},
      {{"jrpc", "call", "0x82001000", "--arg", "bool:maybe"}, "--arg"},
      {{"jrpc", "call", "0x82001000", "--arg", "bytes:ABC"}, "even number"},
      {{"jrpc", "call", "0x82001000", "--arg", "bytes:ZZ"}, "--arg"},
      {{"jrpc", "call", "0x82001000", "--arg", "f32:NaN"}, "NaN"},
      {{"jrpc", "call", "0x82001000", "--arg", "f64:1.5x"}, "--arg"},
      {{"jrpc", "call", "0x82001000", "--arg", "ints:1,,2"}, "--arg"},
      {{"jrpc", "call", "0x82001000", "--arg", "wide:1"}, "unknown type"},
      {{"jrpc", "call", "0"}, "address"},
      {{"jrpc", "call", "0x82001000", "--arg", "str:" + std::string(9000, 'x')}, "line"},
  };
  for (const auto &c : cases) {
    // Without --yes: the request is judged first, so the message is about the request
    // and not about confirmation.
    auto run = cli.run(c.args);
    const bool tooLong = std::string(c.why) == "line";
    CHECK_MSG((run.exit == 2 || (tooLong && run.exit == 1)) && contains(run.err, c.why) && !contains(run.err, "destructive"),
              c.args.back() + "\n" + describe(run));
  }
  CHECK(cli.rig.mock.waitUntilIdle());
  CHECK(cli.rig.mock.commands().empty());
}

TEST(JrpcCli, ARemoteErrorKeepsItsKindInJson) {
  CliRig cli;
  auto unknown = cli.run({"--yes", "--json", "jrpc", "call", "0x99999999"});
  CHECK_MSG(unknown.exit == 1, describe(unknown));
  CHECK_MSG(contains(unknown.out, "\"remote_fault\": \"could_not_resolve\""), describe(unknown));
  CHECK_MSG(contains(unknown.out, "\"code\": \"Io\"") && contains(unknown.out, "error=Could not resolve function address"),
            describe(unknown));
  CHECK_MSG(!contains(unknown.out, "os_error") && !contains(unknown.out, "console_status"), describe(unknown));
  // The console answered, so there is no doubt about delivery.
  CHECK_MSG(!contains(unknown.out, "command_delivery"), describe(unknown));

  cli.rig.mock.registerFunction(kFunction, [](const JrpcCall &) { return JrpcReturn::failure("something odd"); });
  auto other = cli.run({"--yes", "--json", "jrpc", "call", "0x82001000"});
  CHECK_MSG(other.exit == 1 && contains(other.out, "\"remote_fault\": \"other\""), describe(other));
  auto text = cli.run({"--yes", "jrpc", "call", "0x82001000"});
  CHECK_MSG(text.exit == 1 && contains(text.err, "something odd"), describe(text));
  CHECK_MSG(text.out.empty(), describe(text));

  // The existing JSON fields of other failures are unchanged: no remote_fault.
  auto usage = cli.run({"--json", "jrpc", "temp", "fan"});
  CHECK_MSG(usage.exit == 2 && contains(usage.out, "\"code\": \"Usage\"") && !contains(usage.out, "remote_fault"), describe(usage));
}

TEST(JrpcCli, ACallWithoutAnAnswerHasAnUnknownDelivery) {
  CliRig cli;
  cli.returns(JrpcReturn::integer(1));
  cli.rig.mock.inject(JrpcFault::dropConnection().onAddress(kFunction));
  auto run = cli.run({"--yes", "--json", "jrpc", "call", "0x82001000"});
  CHECK_MSG(run.exit == 1, describe(run));
  CHECK_MSG(contains(run.out, "\"command_delivery\": \"unknown\""), describe(run));
  CHECK_MSG(!contains(run.out, "remote_fault"), describe(run));
  CHECK_MSG(contains(run.err, "may have carried the command out"), describe(run));

  // A read-only command reports no delivery.
  cli.rig.mock.inject(JrpcFault::dropConnection().onType(13));
  auto kernel = cli.run({"--json", "jrpc", "kernel"});
  CHECK_MSG(kernel.exit == 1 && contains(kernel.out, "\"error\""), describe(kernel));
  CHECK_MSG(!contains(kernel.out, "command_delivery"), describe(kernel));
  CHECK_MSG(!contains(kernel.err, "may have carried the command out"), describe(kernel));
}

TEST(JrpcCli, ASilentConsoleTimesOutInTheTimeGiven) {
  CliRig cli;
  cli.rig.mock.inject(JrpcFault::silence().onType(13));
  const auto start = std::chrono::steady_clock::now();
  auto run = cli.runWithin(400, {"--json", "jrpc", "kernel"});
  CHECK_MSG(run.exit == 1 && contains(run.out, "\"code\": \"Timeout\""), describe(run));
  CHECK(msSince(start) < 8000);
}

TEST(JrpcCli, NotifyLedsAndConstmem) {
  CliRig cli;
  auto notify = cli.run({"--json", "jrpc", "notify", "Hello", "--type", "3"});
  CHECK_MSG(notify.exit == 0 && contains(notify.out, "\"action\": \"notify\""), describe(notify));
  REQUIRE_EQ(cli.rig.mock.notifications().size(), size_t{1});
  CHECK_EQ(cli.rig.mock.notifications()[0].text, std::string("Hello"));
  CHECK_EQ(cli.rig.mock.notifications()[0].type, uint32_t{3});
  CHECK_EQ(cli.run({"jrpc", "notify", ""}).exit, 2);

  auto refusedLeds = cli.run({"jrpc", "leds", "red", "green", "off", "orange"});
  CHECK_MSG(refusedLeds.exit == 2 && contains(refusedLeds.err, "--yes"), describe(refusedLeds));
  CHECK(cli.rig.mock.ledWrites().empty());
  auto leds = cli.run({"--yes", "--json", "jrpc", "leds", "red", "GREEN", "0", "0x88"});
  CHECK_MSG(leds.exit == 0, describe(leds));
  REQUIRE_EQ(cli.rig.mock.ledWrites().size(), size_t{1});
  const auto write = cli.rig.mock.ledWrites()[0];
  CHECK_EQ(write.topLeft, 0x08);
  CHECK_EQ(write.topRight, 0x80);
  CHECK_EQ(write.bottomLeft, 0);
  CHECK_EQ(write.bottomRight, 0x88);
  CHECK_EQ(cli.run({"--yes", "jrpc", "leds", "red", "green", "off", "purple"}).exit, 2);
  CHECK_EQ(cli.rig.mock.ledWrites().size(), size_t{1});

  auto refusedMem = cli.run({"jrpc", "constmem", "0x82000000", "0xDEADBEEF"});
  CHECK_MSG(refusedMem.exit == 2 && contains(refusedMem.err, "--yes"), describe(refusedMem));
  CHECK(cli.rig.mock.memoryTasks().empty());
  auto mem = cli.run({"--yes", "jrpc", "constmem", "0x82000000", "0xDEADBEEF", "--if-value", "0x1", "--in-title", "0x4D5307E6"});
  CHECK_MSG(mem.exit == 0, describe(mem));
  CHECK_MSG(contains(mem.err, "cannot be removed"), describe(mem));
  REQUIRE_EQ(cli.rig.mock.memoryTasks().size(), size_t{1});
  const auto task = cli.rig.mock.memoryTasks()[0];
  CHECK_EQ(task.address, uint32_t{0x82000000});
  CHECK_EQ(task.value, uint32_t{0xDEADBEEF});
  CHECK_EQ(task.useIf, uint32_t{1});
  CHECK_EQ(task.ifValue, uint32_t{1});
  CHECK_EQ(task.useTitle, uint32_t{1});
  CHECK_EQ(task.titleId, uint32_t{0x4D5307E6});
  CHECK_EQ(cli.run({"--yes", "jrpc", "constmem", "0", "1"}).exit, 2);
  CHECK_EQ(cli.rig.mock.memoryTasks().size(), size_t{1});
  cli.rig.checkCleanTraffic();
}

TEST(JrpcCli, ShutdownNeedsConfirmationAndWorksFromPower) {
  CliRig cli;
  CHECK_EQ(cli.run({"jrpc", "shutdown"}).exit, 2);
  CHECK_EQ(cli.run({"power", "shutdown"}).exit, 2);
  CHECK(cli.rig.mock.events().empty());

  auto direct = cli.run({"--yes", "--json", "jrpc", "shutdown"});
  CHECK_MSG(direct.exit == 0 && contains(direct.out, "\"action\": \"shutdown\"") && contains(direct.out, "\"acknowledged\": false"),
            describe(direct));
  CHECK(cli.rig.mock.waitUntilIdle());
  auto viaPower = cli.run({"--yes", "power", "shutdown"});
  CHECK_MSG(viaPower.exit == 0 && contains(viaPower.out, "Sent shutdown to jrpc://"), describe(viaPower));
  CHECK_EQ(cli.rig.mock.events(), (std::vector<std::string>{"shutdown", "shutdown"}));

  // JRPC has no reboot.
  auto reboot = cli.run({"--yes", "power", "reboot"});
  CHECK_MSG(reboot.exit == 2 && contains(reboot.err, "JRPC"), describe(reboot));
  CHECK_EQ(cli.run({"--yes", "power", "smc-reset"}).exit, 2);
  CHECK_EQ(cli.rig.mock.events().size(), size_t{2});
}

TEST(JrpcCli, RawSendsALineAndPrintsTheReply) {
  CliRig cli;
  const std::string line = "consolefeatures ver=2 type=13 params=\"A\\0\\A\\0\\\"";
  CHECK_EQ(cli.run({"jrpc", "raw", line}).exit, 2);
  auto run = cli.run({"--yes", "--json", "jrpc", "raw", line});
  CHECK_MSG(run.exit == 0 && contains(run.out, "\"reply\": \"17559\"") && contains(run.out, "\"is_error\": false"), describe(run));
  CHECK(contains(cli.run({"--yes", "jrpc", "raw", line}).out, "17559"));
  // An error= line is an answer here.
  auto error = cli.run({"--yes", "--json", "jrpc", "raw", "hello"});
  CHECK_MSG(error.exit == 0 && contains(error.out, "\"is_error\": true") && contains(error.out, "error="), describe(error));
  CHECK_EQ(cli.run({"--yes", "jrpc", "raw", ""}).exit, 2);
  CHECK_EQ(cli.run({"--yes", "jrpc", "raw", "caf\xc3\xa9"}).exit, 2);
  CHECK(cli.rig.mock.waitUntilIdle());
  size_t sent = 0;
  for (const auto &record : cli.rig.mock.commands()) sent += record.name != "bye" ? 1 : 0;
  CHECK_EQ(sent, size_t{3}); // two good lines and "hello"; the refused and the empty ones never left the CLI
}

TEST(JrpcCli, MemoryFileAndNandCommandsRefuseAJrpcTarget) {
  CliRig cli;
  const std::vector<std::vector<std::string>> commands = {
      {"mem", "peek", "0x82000000", "16"}, {"--yes", "mem", "poke", "0x82000000", "1"}, {"mem", "hvpeek", "0x200"},
      {"file", "get", "HDD:\\a", "local"},  {"file", "mkdir", "HDD:\\a"},               {"nand", "badblocks"},
      {"version"},                          {"xbdm", "drives"},                         {"xell", "info"},
  };
  for (const auto &command : commands) {
    auto run = cli.run(command);
    CHECK_MSG(run.exit == 2 && (contains(run.err, "jrpc://") || contains(run.err, "JRPC")), command[0] + "\n" + describe(run));
  }
  auto mem = cli.run({"mem", "peek", "0x82000000", "16"});
  CHECK_MSG(contains(mem.err, "no memory, file, NAND or reboot commands") && contains(mem.err, "xbdm://") &&
                contains(mem.err, "updclient jrpc --help"),
            describe(mem));
  auto xbdm = cli.run({"xbdm", "drives"});
  CHECK_MSG(contains(xbdm.err, "updclient jrpc --help"), describe(xbdm));
  CHECK(cli.rig.mock.waitUntilIdle());
  CHECK(cli.rig.mock.commands().empty());
}

TEST(JrpcCli, TraceRecordsTheLines) {
  CliRig cli;
  ut::TempDir dir;
  const std::string trace = dir.file("trace.txt").string();
  CHECK_EQ(cli.run({"--trace", trace, "jrpc", "kernel"}).exit, 0);
  CHECK_EQ(cli.run({"--trace", trace, "jrpc", "ping"}).exit, 0);
  const std::string text = ut::textOf(ut::readFile(trace).value_or(Bytes{}));
  CHECK_MSG(contains(text, "JRPC trace of jrpc://127.0.0.1:"), text.substr(0, 600));
  CHECK_MSG(contains(text, "< JRPC2 connected"), text.substr(0, 600));
  CHECK_MSG(contains(text, "> consolefeatures ver=2 type=13 "), text.substr(0, 600));
  CHECK(contains(text, "< 17559"));
  CHECK(contains(text, "> Bye"));
  size_t sessions = 0;
  for (size_t at = text.find("# updclient"); at != std::string::npos; at = text.find("# updclient", at + 1)) ++sessions;
  CHECK_EQ(sessions, size_t{2});
  auto unwritable = cli.run({"--trace", dir.file("no/such/dir/t").string(), "jrpc", "kernel"});
  CHECK_MSG(unwritable.exit == 2 && contains(unwritable.err, "trace file"), describe(unwritable));
}

TEST(JrpcCli, HelpNamesJrpcWhereTheTraceAndTargetsAreExplained) {
  auto help = runCli({"--help"});
  CHECK_MSG(help.exit == 0, describe(help));
  CHECK(contains(help.out, "XBDM or JRPC command line"));
  CHECK(contains(help.out, "jrpc://192.168.1.5"));
  CHECK(contains(help.out, "jrpc "));
  auto group = runCli({"jrpc", "--help"});
  CHECK_MSG(group.exit == 0 && contains(group.out, "ping") && contains(group.out, "constmem") && contains(group.out, "raw"), describe(group));
}

TEST(JrpcCli, CtrlCCancelsACallThatNeverReturns) {
  CliRig cli;
  auto gate = std::make_shared<Gate>();
  auto entered = std::make_shared<Gate>();
  Release release(gate);
  cli.rig.mock.registerFunction(kFunction, [gate, entered](const JrpcCall &) {
    entered->open();
    gate->wait(30s);
    return JrpcReturn::integer(1);
  });
  std::chrono::steady_clock::time_point signalled;
  auto run = cli.runWithin(30000, {"--yes", "jrpc", "call", "0x82001000"}, [&](int pid) {
    (void)entered->wait(10s);
    std::this_thread::sleep_for(100ms);
    signalled = std::chrono::steady_clock::now();
#if !defined(_WIN32)
    ::kill(pid, SIGINT);
#else
    (void)pid;
#endif
  });
  CHECK(std::chrono::steady_clock::now() - signalled < 5s);
  CHECK_MSG(run.exit == 1 && contains(run.err, "Cancelled"), describe(run));
}

TEST(JrpcCli, CtrlCDuringInfoAfterSomeFieldsWereReadIsCancelled) {
  CliRig cli;
  // The first four questions are answered; the first temperature never comes in time.
  cli.rig.mock.inject(JrpcFault::delay(20s).onType(15).always());
  std::chrono::steady_clock::time_point signalled;
  auto run = cli.runWithin(30000, {"--json", "jrpc", "info"}, [&](int pid) {
    (void)cli.rig.mock.waitForCommands(5, 10s);
    std::this_thread::sleep_for(100ms);
    signalled = std::chrono::steady_clock::now();
#if !defined(_WIN32)
    ::kill(pid, SIGINT);
#else
    (void)pid;
#endif
  });
  CHECK(std::chrono::steady_clock::now() - signalled < 5s);
  CHECK_MSG(run.exit == 1 && contains(run.err, "Cancelled"), describe(run));
  CHECK_MSG(!contains(run.out, "kernel_version"), describe(run));
}

TEST(JrpcCli, CtrlCWhileConnectingIsCancelled) {
  std::string why;
  auto silent = ut::LoopbackServer::start(
      [](ut::ServerConnection &, const std::atomic<bool> &stop) {
        while (!stop) std::this_thread::sleep_for(10ms);
      },
      &why);
  if (!silent) SKIP("loopback sockets are not available here: " + why);
  std::chrono::steady_clock::time_point signalled;
  auto run = runCli({"--target", "jrpc://127.0.0.1:" + std::to_string(silent->port()), "--timeout-ms", "20000", "jrpc", "ping"},
                    [&](int pid) {
                      for (int i = 0; i < 2500 && !silent->handledConnection(); ++i) std::this_thread::sleep_for(2ms);
                      std::this_thread::sleep_for(100ms);
                      signalled = std::chrono::steady_clock::now();
#if !defined(_WIN32)
                      ::kill(pid, SIGINT);
#else
                      (void)pid;
#endif
                    });
  CHECK(std::chrono::steady_clock::now() - signalled < 3s);
  CHECK_MSG(run.exit == 1 && contains(run.err, "Cancelled"), describe(run));
}
