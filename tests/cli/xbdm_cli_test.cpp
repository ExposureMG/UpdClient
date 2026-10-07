#include "protocols/xbdm/integration_support.hpp"
#include "support/loopback_server.hpp"

#include <core/hex.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
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

// The updclient executable against the mock over loopback TCP: the XBDM side of
// the file, mem, power and info commands, the xbdm group, --trace and Ctrl-C.

using namespace xit;

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
  return "exit " + std::to_string(run.exit) + "\nstdout: " + run.out.substr(0, 400) + "\nstderr: " + run.err.substr(0, 600);
}

class CliRig {
public:
  CliRig() : rig(Link::Tcp) {}
  Rig rig;
  std::string target() const { return "xbdm://127.0.0.1:" + std::to_string(rig.port()); }
  Run run(std::vector<std::string> args, const std::function<void(int)> &during = {}) {
    args.insert(args.begin(), {"--target", target(), "--timeout-ms", "3000"});
    return runCli(args, during);
  }
};

} // namespace

TEST(XbdmCli, Info) {
  CliRig cli;
  auto text = cli.run({"info"});
  CHECK_MSG(text.exit == 0, describe(text));
  CHECK_MSG(contains(text.out, "MockDevkit"), describe(text));
  CHECK(contains(text.out, "192.168.1.2"));
  auto json = cli.run({"--json", "info"});
  CHECK_MSG(json.exit == 0, describe(json));
  CHECK_MSG(contains(json.out, "\"debug_name\": \"MockDevkit\""), describe(json));
  CHECK(contains(json.out, "\"console_type\": \"devkit\""));
  cli.rig.checkCleanTraffic();
}

TEST(XbdmCli, ATargetMayBeAHostName) {
  CliRig cli;
  const std::string localhost = "xbdm://localhost:" + std::to_string(cli.rig.port());
  auto run = runCli({"--target", localhost, "--timeout-ms", "3000", "info"});
  if (run.exit != 0 && contains(run.err, "cannot resolve")) SKIP("'localhost' does not resolve here");
  CHECK_MSG(run.exit == 0, describe(run));
  CHECK_MSG(contains(run.out, "MockDevkit"), describe(run));
}

TEST(XbdmCli, ConsoleTextCannotReachTheTerminalAsEscapeSequences) {
  CliRig cli;
  auto info = cli.rig.mock.info();
  info.debugName = "\x1b]0;title\x07\x1b[2Jname";
  info.consoleType = "dev\x7fkit\xe9";
  cli.rig.mock.setInfo(info);
  auto text = cli.run({"info"});
  CHECK_MSG(text.exit == 0, describe(text));
  CHECK(!contains(text.out, "\x1b"));
  CHECK(!contains(text.out, "\x07"));
  CHECK(!contains(text.out, "\x7f"));
  CHECK_MSG(contains(text.out, "\\x1b]0;title\\x07\\x1b[2Jname"), describe(text));
  CHECK_MSG(contains(text.out, "dev\\x7fkit\\xe9"), describe(text));
  auto raw = cli.run({"--yes", "xbdm", "raw", "dbgname"});
  CHECK_MSG(raw.exit == 0, describe(raw));
  CHECK(!contains(raw.out, "\x1b"));
  CHECK_MSG(contains(raw.out, "200- \\x1b]0;title"), describe(raw));
  // JSON carries the text as it is, escaped by JSON.
  auto json = cli.run({"--json", "info"});
  CHECK_MSG(contains(json.out, "\\u001b]0;title\\u0007"), describe(json));
}

TEST(XbdmCli, FileGetSendAndMkdir) {
  CliRig cli;
  ut::TempDir dir;
  const auto local = dir.file("default.xex").string();
  auto get = cli.run({"--json", "file", "get", "HDD:\\default.xex", local});
  CHECK_MSG(get.exit == 0, describe(get));
  CHECK(contains(get.out, "\"bytes\": 20000"));
  CHECK_EQ(ut::readFile(local).value_or(Bytes{}), *cli.rig.mock.fileData("HDD:\\default.xex"));

  const Bytes data = ut::patternBytes(300000, 3);
  REQUIRE(ut::writeFile(dir.file("up.bin"), data));
  auto send = cli.run({"file", "send", dir.file("up.bin").string(), "/HDD/Content/up.bin"});
  CHECK_MSG(send.exit == 0, describe(send));
  CHECK_EQ(cli.rig.mock.fileData("HDD:\\Content\\up.bin").value_or(Bytes{}), data);
  CHECK_MSG(contains(send.err, "Upload: 100%"), describe(send));
  CHECK(partFiles(cli.rig.mock, "HDD:\\Content").empty());

  auto mkdir = cli.run({"file", "mkdir", "HDD:\\NewFolder"});
  CHECK_MSG(mkdir.exit == 0, describe(mkdir));
  CHECK(cli.rig.mock.entry("HDD:\\NewFolder").has_value());

  auto missing = cli.run({"--json", "file", "get", "HDD:\\missing.bin", dir.file("m").string()});
  CHECK_EQ(missing.exit, 1);
  CHECK_MSG(contains(missing.out, "\"console_status\": 402"), describe(missing));
  CHECK(!contains(missing.out, "os_error"));
  CHECK(!std::filesystem::exists(dir.file("m")));
  CHECK(!std::filesystem::exists(dir.file("m.part")));

  auto badPath = cli.run({"file", "get", "HDD:/x", dir.file("m").string()});
  CHECK_EQ(badPath.exit, 2);
  auto unsupported = cli.run({"file", "mount", "Hdd", "\\Device\\Harddisk0\\Partition1"});
  CHECK_MSG(unsupported.exit == 2 && contains(unsupported.err, "XBDM"), describe(unsupported));
  auto nand = cli.run({"nand", "badblocks"});
  CHECK_EQ(nand.exit, 2);
}

TEST(XbdmCli, MemoryPeekAndPoke) {
  CliRig cli;
  auto peek = cli.run({"--json", "mem", "peek", "0x82000000", "16"});
  CHECK_MSG(peek.exit == 0, describe(peek));
  CHECK(contains(peek.out, updclient::formatHex(*cli.rig.mock.memory(0x82000000u, 16))));
  auto edge = cli.run({"mem", "peek", "0x30000ffc", "8"});
  CHECK_MSG(edge.exit == 0 && contains(edge.out, "????????"), describe(edge));

  auto refused = cli.run({"mem", "poke", "0x82000000", "0xDEADBEEF"});
  CHECK_EQ(refused.exit, 2);
  auto poke = cli.run({"--yes", "mem", "poke", "0x82000000", "0xDEADBEEF"});
  CHECK_MSG(poke.exit == 0, describe(poke));
  CHECK_EQ(*cli.rig.mock.memory(0x82000000u, 4), (Bytes{0xDE, 0xAD, 0xBE, 0xEF}));
  auto hv = cli.run({"mem", "hvpeek", "0x200"});
  CHECK_EQ(hv.exit, 2);
}

TEST(XbdmCli, PowerAndLaunch) {
  CliRig cli;
  CHECK_EQ(cli.run({"power", "reboot"}).exit, 2);
  auto reboot = cli.run({"--yes", "power", "reboot"});
  CHECK_MSG(reboot.exit == 0 && contains(reboot.out, "acknowledged"), describe(reboot));
  auto cold = cli.run({"--yes", "xbdm", "reboot", "--cold"});
  CHECK_MSG(cold.exit == 0 && contains(cold.out, "closed the connection"), describe(cold));
  auto launch = cli.run({"--yes", "xbdm", "launch", "/HDD/Games/Mock/default.xex"});
  CHECK_MSG(launch.exit == 0, describe(launch));
  auto off = cli.run({"--yes", "power", "shutdown"});
  CHECK_MSG(off.exit == 0, describe(off));
  CHECK_EQ(cli.run({"--yes", "power", "smc-reset"}).exit, 2);
  CHECK_EQ(cli.rig.mock.events(),
           (std::vector<std::string>{"magicboot", "magicboot cold",
                                     "magicboot title=HDD:\\Games\\Mock\\default.xex directory=HDD:\\Games\\Mock",
                                     "shutdown"}));
}

TEST(XbdmCli, XbdmGroup) {
  CliRig cli;
  auto ls = cli.run({"--json", "xbdm", "ls", "/HDD"});
  CHECK_MSG(ls.exit == 0 && contains(ls.out, "\"name\": \"Content\""), describe(ls));
  auto lsText = cli.run({"xbdm", "ls", "HDD:\\"});
  CHECK_MSG(lsText.exit == 0 && contains(lsText.out, "default.xex"), describe(lsText));
  auto stat = cli.run({"--json", "xbdm", "stat", "HDD:\\default.xex"});
  CHECK_MSG(stat.exit == 0 && contains(stat.out, "\"size\": 20000"), describe(stat));
  auto drives = cli.run({"xbdm", "drives"});
  CHECK_MSG(drives.exit == 0 && contains(drives.out, "250000000000"), describe(drives));
  auto mv = cli.run({"xbdm", "mv", "HDD:\\default.xex", "HDD:\\Content\\moved.xex"});
  CHECK_MSG(mv.exit == 0, describe(mv));
  CHECK(cli.rig.mock.entry("HDD:\\Content\\moved.xex").has_value());
  CHECK_EQ(cli.run({"xbdm", "rm", "HDD:\\Content\\moved.xex"}).exit, 2);
  CHECK(cli.rig.mock.entry("HDD:\\Content\\moved.xex").has_value());
  auto rm = cli.run({"--yes", "xbdm", "rm", "HDD:\\Content\\moved.xex"});
  CHECK_MSG(rm.exit == 0, describe(rm));
  CHECK(!cli.rig.mock.entry("HDD:\\Content\\moved.xex").has_value());
  auto rmdir = cli.run({"--yes", "xbdm", "rm", "--dir", "HDD:\\Empty"});
  CHECK_MSG(rmdir.exit == 0, describe(rmdir));
  auto notEmpty = cli.run({"--yes", "--json", "xbdm", "rm", "--dir", "HDD:\\Content"});
  CHECK_MSG(notEmpty.exit == 1 && contains(notEmpty.out, "\"console_status\": 411"), describe(notEmpty));
  CHECK_MSG(cli.run({"xbdm", "modules"}).out.find("xboxkrnl.exe") != std::string::npos, "modules");
  CHECK_MSG(cli.run({"xbdm", "regions"}).out.find("82000000") != std::string::npos, "regions");
  CHECK_EQ(cli.run({"xbdm", "eject"}).exit, 0);
  CHECK_EQ(cli.run({"xbdm", "raw", "dbgname"}).exit, 2);
  auto raw = cli.run({"--yes", "xbdm", "raw", "dirlist name=\"HDD:\\Attrs\""});
  CHECK_MSG(raw.exit == 0 && contains(raw.out, "202- multiline response follows\nname=\"ro.txt\""), describe(raw));
  auto rawJson = cli.run({"--yes", "--json", "xbdm", "raw", "systeminfo"});
  CHECK_MSG(rawJson.exit == 0 && contains(rawJson.out, "\"status\": 407"), describe(rawJson));

  ut::TempDir dir;
  auto shot = cli.run({"xbdm", "screenshot", "-o", dir.file("shot.raw").string()});
  CHECK_MSG(shot.exit == 0, describe(shot));
  CHECK_EQ(ut::readFile(dir.file("shot.raw")).value_or(Bytes{}), cli.rig.mock.screenshot().framebuffer);

  auto bare = runCli({"--ip", "127.0.0.1", "--port", std::to_string(cli.rig.port()), "xbdm", "drives"});
  CHECK_MSG(bare.exit == 0 && contains(bare.out, "HDD:"), describe(bare));
  cli.rig.checkCleanTraffic();
}

TEST(XbdmCli, TraceRecordsLinesButNoFileData) {
  CliRig cli;
  ut::TempDir dir;
  const std::string trace = dir.file("trace.txt").string();
  REQUIRE_OK(cli.rig.mock.addFile("HDD:\\secret.bin", ut::bytesOf(std::string(5000, 'Q') + "SECRETMARKER")));
  REQUIRE(ut::writeFile(dir.file("up"), ut::bytesOf("UPLOADMARKER-" + std::string(3000, 'U'))));
  CHECK_EQ(cli.run({"--trace", trace, "file", "get", "HDD:\\secret.bin", dir.file("s").string()}).exit, 0);
  CHECK_EQ(cli.run({"--trace", trace, "file", "send", dir.file("up").string(), "HDD:\\up.bin"}).exit, 0);
  CHECK_EQ(cli.run({"--trace", trace, "xbdm", "ls", "HDD:\\"}).exit, 0);
  const std::string text = ut::textOf(ut::readFile(trace).value_or(Bytes{}));
  CHECK_MSG(contains(text, "> getfileattributes name=\"HDD:\\secret.bin\""), text.substr(0, 800));
  CHECK(contains(text, "< 201- connected"));
  CHECK(contains(text, "< 203- binary response follows"));
  CHECK(contains(text, "< [5016 bytes of binary data]"));
  CHECK(contains(text, "> [3013 bytes of binary data]"));
  CHECK(contains(text, "< 204- send binary data"));
  CHECK(contains(text, "name=\"Content\""));
  CHECK(contains(text, "> bye"));
  CHECK(!contains(text, "SECRETMARKER"));
  CHECK(!contains(text, "UPLOADMARKER"));
  CHECK(!contains(text, "QQQQ"));
  size_t sessions = 0;
  for (size_t at = text.find("# updclient"); at != std::string::npos; at = text.find("# updclient", at + 1)) ++sessions;
  CHECK_EQ(sessions, size_t{3});
}

TEST(XbdmCli, CtrlCCancelsADownload) {
  CliRig cli;
  ut::TempDir dir;
  REQUIRE_OK(cli.rig.mock.addFile("HDD:\\slow.bin", ut::patternBytes(400000, 1)));
  cli.rig.mock.inject(XbdmFault::stall(30 + 4 + 100000).on("getfile"));
  const auto local = dir.file("slow.bin");
  std::chrono::steady_clock::time_point signalled;
  auto run = cli.run({"file", "get", "HDD:\\slow.bin", local.string()}, [&](int pid) {
    for (int i = 0; i < 2500 && cli.rig.linesNamed("getfile").empty(); ++i) std::this_thread::sleep_for(2ms);
    std::this_thread::sleep_for(200ms);
    signalled = std::chrono::steady_clock::now();
#if !defined(_WIN32)
    ::kill(pid, SIGINT);
#else
    (void)pid;
#endif
  });
  CHECK(std::chrono::steady_clock::now() - signalled < 5s);
  CHECK_MSG(run.exit == 1 && contains(run.err, "Cancelled"), describe(run));
  CHECK(dir.entries().empty());
}

TEST(XbdmCli, CtrlCCancelsAnUploadAndDeletesTheTemporaryFile) {
  CliRig cli;
  ut::TempDir dir;
  REQUIRE(ut::writeFile(dir.file("big"), ut::patternBytes(24u << 20, 2)));
  cli.rig.mock.inject(XbdmFault::stallUploadAfterBytes(4096).on("sendfile"));
  auto run = cli.run({"file", "send", dir.file("big").string(), "HDD:\\big.bin"}, [&](int pid) {
    for (int i = 0; i < 2500 && cli.rig.mock.uploads().empty() && partFiles(cli.rig.mock, "HDD:\\").empty(); ++i) {
      std::this_thread::sleep_for(2ms);
    }
    std::this_thread::sleep_for(300ms);
#if !defined(_WIN32)
    ::kill(pid, SIGINT);
#else
    (void)pid;
#endif
  });
  CHECK_MSG(run.exit == 1 && contains(run.err, "Cancelled"), describe(run));
  CHECK_MSG(contains(run.err, "Deleted the unfinished upload"), describe(run));
  CHECK(partFiles(cli.rig.mock, "HDD:\\").empty());
  CHECK(!cli.rig.mock.entry("HDD:\\big.bin").has_value());
}

TEST(XbdmCli, AKeptUploadIsNamedInTheJsonError) {
  CliRig cli;
  ut::TempDir dir;
  REQUIRE(ut::writeFile(dir.file("new"), ut::bytesOf("new contents")));
  cli.rig.mock.inject(XbdmFault::refusal("414- access denied").on("rename"));
  auto run = cli.run({"--json", "file", "send", dir.file("new").string(), "HDD:\\default.xex"});
  CHECK_MSG(run.exit == 1, describe(run));
  const auto parts = partFiles(cli.rig.mock, "HDD:\\");
  REQUIRE_EQ(parts.size(), size_t{1});
  CHECK_MSG(contains(run.out, "\"kept_upload\": \"HDD:\\\\" + parts[0] + "\""), describe(run));
  CHECK_MSG(contains(run.err, "rename it with 'xbdm mv'"), describe(run));
  CHECK(!cli.rig.mock.entry("HDD:\\default.xex").has_value());
}

TEST(XbdmCli, ADeleteWithoutAnAnswerHasAnUnknownDelivery) {
  CliRig cli;
  cli.rig.mock.inject(XbdmFault::dropAfterBytes(0).on("delete"));
  auto run = cli.run({"--json", "--yes", "xbdm", "rm", "HDD:\\default.xex"});
  CHECK_MSG(run.exit == 1, describe(run));
  CHECK_MSG(contains(run.out, "\"command_delivery\": \"unknown\""), describe(run));
  CHECK_MSG(contains(run.err, "may have carried the command out"), describe(run));

  // A refusal is an answer: no delivery field.
  auto refused = cli.run({"--json", "--yes", "xbdm", "rm", "HDD:\\no such file"});
  CHECK_MSG(refused.exit == 1, describe(refused));
  CHECK_MSG(!contains(refused.out, "command_delivery"), describe(refused));
}

TEST(XbdmCli, CtrlCWhileConnectingIsCancelled) {
  std::string why;
  auto silent = ut::LoopbackServer::start(
      [](ut::ServerConnection &, const std::atomic<bool> &stop) {
        while (!stop) std::this_thread::sleep_for(10ms);
      },
      &why);
  if (!silent) SKIP("loopback sockets are not available here: " + why);
  std::chrono::steady_clock::time_point signalled;
  auto run = runCli({"--target", "xbdm://127.0.0.1:" + std::to_string(silent->port()), "--timeout-ms", "20000",
                     "xbdm", "ls", "HDD:\\"},
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
  CHECK(std::chrono::steady_clock::now() - signalled < 2s);
  CHECK_MSG(run.exit == 1 && contains(run.err, "Cancelled"), describe(run));
}

TEST(XbdmCli, CtrlCDuringTheCleanupReconnectWarnsAboutTheUpload) {
  CliRig cli;
  ut::TempDir dir;
  REQUIRE(ut::writeFile(dir.file("big"), ut::patternBytes(1u << 20, 3)));
  cli.rig.mock.inject(XbdmFault::dropUploadAfterBytes(4096).on("sendfile"));
  // The reconnect that would delete the temporary file never gets its greeting.
  cli.rig.mock.inject(XbdmFault::silence().onGreeting().onConnection(1));
  std::chrono::steady_clock::time_point signalled;
  auto run = cli.run({"file", "send", dir.file("big").string(), "HDD:\\big.bin"}, [&](int pid) {
    for (int i = 0; i < 2500 && cli.rig.mock.connectionsAccepted() < 2; ++i) std::this_thread::sleep_for(2ms);
    std::this_thread::sleep_for(100ms);
    signalled = std::chrono::steady_clock::now();
#if !defined(_WIN32)
    ::kill(pid, SIGINT);
#else
    (void)pid;
#endif
  });
  CHECK(std::chrono::steady_clock::now() - signalled < 2s);
  CHECK_MSG(run.exit == 1, describe(run));
  CHECK_MSG(contains(run.err, "is still on the console; delete it with 'xbdm rm'"), describe(run));
  CHECK_EQ(partFiles(cli.rig.mock, "HDD:\\").size(), size_t{1});
}

TEST(XbdmCli, CtrlCDuringDiscoveryPrintsWhatWasFound) {
  std::chrono::steady_clock::time_point signalled;
  auto run = runCli({"--discovery-timeout-ms", "20000", "discover", "--protocol", "xbdm"}, [&](int pid) {
    std::this_thread::sleep_for(300ms);
    signalled = std::chrono::steady_clock::now();
#if !defined(_WIN32)
    ::kill(pid, SIGINT);
#else
    (void)pid;
#endif
  });
  if (run.exit == 3 && contains(run.err, "unavailable")) SKIP("UDP broadcast is not available here");
  CHECK(std::chrono::steady_clock::now() - signalled < 2s);
  CHECK_MSG(run.exit == 1 && contains(run.err, "Cancelled"), describe(run));
}
