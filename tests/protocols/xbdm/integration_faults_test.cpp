#include "protocols/xbdm/integration_support.hpp"

#include <thread>

// The mock's fault injection modes (section 5.1, item 10) against the client's
// obligations (section 5.2): every misbehaviour ends in a clean error, never in a
// hang, a crash or bytes read as the answer to a later command.

using namespace xit;
using updclient::xbdm::consoleStatusCode;

namespace {

ClientOptions impatient() {
  ClientOptions o = quickOptions();
  o.greetingTimeout = 300ms;
  o.idleTimeout = 300ms;
  o.slowIdleTimeout = 300ms;
  return o;
}

} // namespace

XBDM_LINK_TEST(XbdmFaultIntegration, GreetingFaults) {
  Rig rig(link);
  rig.mock.inject(XbdmFault::silence().onGreeting().repeat(1));
  auto silent = rig.open(impatient());
  CHECK_ERR(silent, ErrorCode::Timeout);

  rig.mock.inject(XbdmFault::statusLine("401- max number of connections exceeded").onGreeting());
  auto full = rig.open(impatient());
  CHECK_ERR(full, ErrorCode::LimitExceeded);
  if (!full) CHECK_EQ(consoleStatusCode(full.error()).value_or(0), 401);

  rig.mock.inject(XbdmFault::statusLine("200- hello").onGreeting());
  CHECK_ERR(rig.open(impatient()), ErrorCode::Protocol);
  rig.mock.inject(XbdmFault::statusLine("garbage").onGreeting());
  CHECK_ERR(rig.open(impatient()), ErrorCode::Protocol);
  rig.mock.inject(XbdmFault::dropAfterBytes(7).onGreeting());
  CHECK_ERR(rig.open(impatient()), ErrorCode::Disconnected);
  rig.mock.inject(XbdmFault::statusLine("201- CONNECTED").onGreeting());
  CHECK_OK(rig.open(impatient()));
  rig.mock.inject(XbdmFault::trickleBytes().onGreeting());
  auto trickled = rig.open(impatient());
  REQUIRE_OK(trickled);
  CHECK_OK(trickled->debugName());
}

XBDM_LINK_TEST(XbdmFaultIntegration, SilenceAfterTheGreeting) {
  Rig rig(link);
  rig.mock.inject(XbdmFault::silence());
  auto client = rig.client(impatient());
  const auto start = std::chrono::steady_clock::now();
  auto r = client.debugName();
  const auto took = std::chrono::steady_clock::now() - start;
  CHECK_ERR(r, ErrorCode::Timeout);
  if (!r) CHECK(r.error().message.find("stopped responding") != std::string::npos);
  CHECK(took >= 250ms);
  CHECK(took < 3s);
  CHECK(!client.isConnected());
  CHECK_ERR(client.list("HDD:\\"), ErrorCode::NotConnected);
}

XBDM_LINK_TEST(XbdmFaultIntegration, SilenceInsideAnswers) {
  Rig rig(link);
  const size_t head202 = std::string("202- multiline response follows\r\n").size();
  const size_t head203 = std::string("203- binary response follows\r\n").size();
  struct Case {
    const char *command;
    size_t after;
  };
  for (const Case c : {Case{"dirlist", 3}, Case{"dirlist", head202}, Case{"dirlist", head202 + 20},
                       Case{"getfile", head203}, Case{"getfile", head203 + 2}, Case{"getfile", head203 + 4 + 100},
                       Case{"getmemex", head203 + 1}, Case{"getmemex", head203 + 2 + 10},
                       Case{"screenshot", head203 + 10}, Case{"drivelist", head202 + 5}}) {
    rig.mock.inject(XbdmFault::stall(c.after).on(c.command));
    auto client = rig.client(impatient());
    updclient::Result<void> r;
    const std::string name = c.command;
    if (name == "dirlist") r = asVoid(client.list("HDD:\\"));
    else if (name == "getfile") {
      ut::TempDir dir;
      r = client.downloadToFile("HDD:\\default.xex", dir.file("x"));
      CHECK(dir.entries().empty());
    } else if (name == "getmemex") r = asVoid(client.getMemoryEx(0x82000000u, 0x800));
    else if (name == "screenshot") r = asVoid(client.screenshot());
    else r = asVoid(client.drives());
    CHECK_MSG(!r && r.error().code == ErrorCode::Timeout,
              name + " at " + std::to_string(c.after) + ": " + (r ? "ok" : updclient::formatError(r.error())));
    CHECK(!client.isConnected());
  }
}

XBDM_LINK_TEST(XbdmFaultIntegration, CloseInsideAnswers) {
  Rig rig(link);
  for (const char *command : {"dbgname", "drivelist", "dirlist", "getmemex", "screenshot", "getmem", "modules"}) {
    for (size_t k : {size_t{0}, size_t{1}, size_t{4}, size_t{5}, size_t{16}}) {
      rig.mock.inject(XbdmFault::dropAfterBytes(k).on(command));
      auto client = rig.client();
      const std::string name = command;
      updclient::Result<void> r;
      if (name == "dbgname") r = asVoid(client.debugName());
      else if (name == "drivelist") r = asVoid(client.drives());
      else if (name == "dirlist") r = asVoid(client.list("HDD:\\"));
      else if (name == "getmemex") r = asVoid(client.getMemoryEx(0x82000000u, 0x40));
      else if (name == "screenshot") r = asVoid(client.screenshot());
      else if (name == "getmem") r = asVoid(client.getMemory(0x82000000u, 0x40));
      else r = asVoid(client.modules());
      CHECK_MSG(!r && r.error().code == ErrorCode::Disconnected,
                name + " at " + std::to_string(k) + ": " + (r ? "ok" : updclient::formatError(r.error())));
      CHECK(!client.isConnected());
    }
  }
}

XBDM_LINK_TEST(XbdmFaultIntegration, ByteByByteAnswers) {
  Rig rig(link);
  rig.mock.inject(XbdmFault::trickleBytes().always());
  auto client = rig.client();
  CHECK_OK(client.consoleInfo());
  auto listing = client.list("HDD:\\");
  REQUIRE_OK(listing);
  CHECK_EQ(listing->entries.size(), size_t{6});
  auto memory = client.getMemoryEx(0x82000000u, 0x900);
  REQUIRE_OK(memory);
  CHECK_EQ(memory->data, *rig.mock.memory(0x82000000u, 0x900));
  ut::TempDir dir;
  REQUIRE_OK(client.downloadToFile("HDD:\\Games\\Mock\\default.xex", dir.file("x")));
  CHECK_EQ(*ut::readFile(dir.file("x")), ut::patternBytes(4096, 8));
  REQUIRE(ut::writeFile(dir.file("y"), ut::patternBytes(3000, 1)));
  REQUIRE_OK(client.uploadFromFile(dir.file("y"), "HDD:\\y.bin"));
  CHECK_EQ(*rig.mock.fileData("HDD:\\y.bin"), ut::patternBytes(3000, 1));
  CHECK_OK(client.screenshot());
}

XBDM_LINK_TEST(XbdmFaultIntegration, UnknownStatusCodesDropTheConnection) {
  Rig rig(link);
  for (const char *line : {"299- odd", "500- internal error", "abc", "1xx- early", "300- redirect", "100- continue",
                           "20- short", "2000- long", "", "-", "999- nine"}) {
    rig.mock.inject(XbdmFault::statusLine(line).on("dbgname"));
    auto client = rig.client();
    auto r = client.debugName();
    CHECK_MSG(!r && r.error().code == ErrorCode::Protocol,
              std::string(line) + ": " + (r ? "ok '" + *r + "'" : updclient::formatError(r.error())));
    CHECK(!client.isConnected());
  }
}

XBDM_LINK_TEST(XbdmFaultIntegration, UnexpectedSuccessCodesDropTheConnection) {
  Rig rig(link);
  struct Case {
    const char *command;
    const char *reply;
  };
  for (const Case c : {Case{"dbgname", "203- binary response follows\r\n"},
                       Case{"dbgname", "202- multiline response follows\r\nx\r\n.\r\n"},
                       Case{"dirlist", "200- OK\r\n"}, Case{"getfile", "200- OK\r\n"},
                       Case{"getmemex", "202- multiline response follows\r\n.\r\n"},
                       Case{"getmem", "203- binary response follows\r\n"}, Case{"mkdir", "204- send binary data\r\n"},
                       Case{"sendfile", "200- OK\r\n"}, Case{"screenshot", "200- OK\r\n"}}) {
    rig.mock.inject(XbdmFault::reply(c.reply).on(c.command));
    auto client = rig.client();
    const std::string name = c.command;
    updclient::Result<void> r;
    if (name == "dbgname") r = asVoid(client.debugName());
    else if (name == "dirlist") r = asVoid(client.list("HDD:\\"));
    else if (name == "getfile") r = asVoid(client.openRead("HDD:\\default.xex"));
    else if (name == "getmemex") r = asVoid(client.getMemoryEx(0x82000000u, 4));
    else if (name == "getmem") r = asVoid(client.getMemory(0x82000000u, 4));
    else if (name == "mkdir") r = client.makeDirectory("HDD:\\m");
    else if (name == "sendfile") r = asVoid(client.openWrite("HDD:\\s.bin", 10));
    else r = asVoid(client.screenshot());
    CHECK_MSG(!r && r.error().code == ErrorCode::Protocol,
              name + ": " + (r ? "ok" : updclient::formatError(r.error())));
    CHECK(!client.isConnected());
  }
}

XBDM_LINK_TEST(XbdmFaultIntegration, GetfileLengthClaims) {
  Rig rig(link);
  ut::TempDir dir;
  REQUIRE_OK(rig.mock.addFile("HDD:\\c.bin", ut::patternBytes(1000, 1)));

  rig.mock.inject(XbdmFault::claimLength(1100).on("getfile"));
  auto client = rig.client(impatient());
  auto bounded = client.openRead("HDD:\\c.bin", 1000);
  CHECK_ERR(bounded, ErrorCode::LimitExceeded);
  CHECK(!client.isConnected());

  rig.mock.inject(XbdmFault::claimLength(1100).on("getfile"));
  REQUIRE_OK(client.reconnect());
  auto stalls = client.downloadToFile("HDD:\\c.bin", dir.file("c"));
  CHECK(!stalls);
  CHECK(dir.entries().empty());

  rig.mock.inject(XbdmFault::claimLength(0xFFFFFFFFull).on("getfile"));
  REQUIRE_OK(client.reconnect());
  auto options = client.options();
  options.maxDownloadBytes = 1u << 20;
  client.setOptions(options);
  CHECK_ERR(client.openRead("HDD:\\c.bin"), ErrorCode::LimitExceeded);

  rig.mock.inject(XbdmFault::claimLength(900).on("getfile"));
  REQUIRE_OK(client.reconnect());
  auto shorter = client.openRead("HDD:\\c.bin");
  REQUIRE_OK(shorter);
  CHECK_EQ(shorter->size(), 900u);
  Bytes all(900);
  size_t got = 0;
  while (got < 900) {
    auto n = shorter->read(std::span<uint8_t>(all).subspan(got));
    REQUIRE_OK(n);
    if (*n == 0) break;
    got += *n;
  }
  CHECK_EQ(got, size_t{900});
  // The 100 bytes the console sent beyond its own length belong to no command.
  auto next = client.debugName();
  CHECK_ERR(next, ErrorCode::Protocol);
}

XBDM_LINK_TEST(XbdmFaultIntegration, ScreenshotSizeClaims) {
  Rig rig(link);
  const auto shot = rig.mock.screenshot();
  rig.mock.inject(XbdmFault::claimLength(65ull << 20).on("screenshot"));
  auto client = rig.client(impatient());
  CHECK_ERR(client.screenshot(), ErrorCode::LimitExceeded);
  rig.mock.inject(XbdmFault::claimLength(shot.framebuffer.size() * 3).on("screenshot"));
  REQUIRE_OK(client.reconnect());
  CHECK_ERR(client.screenshot(), ErrorCode::Protocol);
  rig.mock.inject(XbdmFault::reply("203- binary response follows\r\npitch=0x100 width=zz\r\n").on("screenshot"));
  REQUIRE_OK(client.reconnect());
  CHECK_ERR(client.screenshot(), ErrorCode::Protocol);
}

XBDM_LINK_TEST(XbdmFaultIntegration, GetmemexBlockHeaders) {
  Rig rig(link);
  auto client = rig.client();
  rig.mock.inject(XbdmFault::memexBlockHeader(0x0400).on("getmemex"));
  CHECK_ERR(client.getMemoryEx(0x82000000u, 16), ErrorCode::Protocol);
  CHECK(!client.isConnected());

  REQUIRE_OK(client.reconnect());
  rig.mock.inject(XbdmFault::memexBlockHeader(0x8000).on("getmemex"));
  auto none = client.getMemoryEx(0x82000000u, 16);
  REQUIRE_OK(none);
  CHECK_EQ(none->readableBytes(), size_t{0});
  CHECK(client.isConnected());

  rig.mock.inject(XbdmFault::memexBlockHeader(0x0000).on("getmemex"));
  CHECK_ERR(client.getMemoryEx(0x82000000u, 16), ErrorCode::Protocol);

  REQUIRE_OK(client.reconnect());
  rig.mock.inject(XbdmFault::memexBlockHeader(0x8008).on("getmemex"));
  auto half = client.getMemoryEx(0x82000000u, 16);
  REQUIRE_OK(half);
  CHECK_EQ(half->readableBytes(), size_t{8});

  rig.mock.inject(XbdmFault::memexBlockHeader(0xFFFF).on("getmemex"));
  CHECK_ERR(client.getMemoryEx(0x82000000u, 0x100), ErrorCode::Protocol);
}

XBDM_LINK_TEST(XbdmFaultIntegration, OverlongAndEndlessLines) {
  Rig rig(link);
  auto client = rig.client();
  rig.mock.inject(XbdmFault::oversizedLine().on("dbgname"));
  CHECK_ERR(client.debugName(), ErrorCode::LimitExceeded);
  CHECK(!client.isConnected());
  REQUIRE_OK(client.reconnect());
  rig.mock.inject(XbdmFault::oversizedLine(64 * 1024).on("dbgname"));
  auto atLimit = client.debugName();
  REQUIRE_OK(atLimit);
  CHECK_EQ(atLimit->size(), size_t{64 * 1024 - 5});
  rig.mock.inject(XbdmFault::endlessLine().on("dbgname"));
  CHECK_ERR(client.debugName(), ErrorCode::LimitExceeded);
  REQUIRE_OK(client.reconnect());

  std::string body = "202- multiline response follows\r\n";
  for (int i = 0; i < 300; ++i) body += "name=\"" + std::string(60000, 'f') + "\" sizehi=0x0 sizelo=0x0\r\n";
  body += ".\r\n";
  rig.mock.inject(XbdmFault::reply(body).on("dirlist"));
  CHECK_ERR(client.list("HDD:\\"), ErrorCode::LimitExceeded);
}

XBDM_LINK_TEST(XbdmFaultIntegration, BytesThatBelongToNoCommand) {
  Rig rig(link);
  auto client = rig.client();
  rig.mock.inject(XbdmFault::reply("200- MockDevkit\r\n200- stale\r\n").on("dbgname"));
  REQUIRE_OK(client.debugName());
  auto next = client.consoleType();
  CHECK_ERR(next, ErrorCode::Protocol);
  CHECK(!client.isConnected());

  REQUIRE_OK(client.reconnect());
  rig.mock.inject(XbdmFault::reply("202- multiline response follows\r\n.\r\nextra").on("drivelist"));
  REQUIRE_OK(client.drives());
  CHECK_ERR(client.debugName(), ErrorCode::Protocol);
}

XBDM_LINK_TEST(XbdmFaultIntegration, DroppedWhileIdle) {
  Rig rig(link);
  auto client = rig.client();
  REQUIRE_OK(client.debugName());
  rig.mock.dropAllConnections();
  REQUIRE(rig.mock.waitForActiveConnections(0));
  auto r = client.debugName();
  CHECK(!r);
  if (!r) CHECK(r.error().code == ErrorCode::Disconnected || r.error().code == ErrorCode::Io);
  CHECK(!client.isConnected());
  REQUIRE_OK(client.reconnect());
  CHECK_OK(client.debugName());
}

// Deterministic damage to every kind of answer: the client must come back with a
// result or an error each time, and a reconnect must always work afterwards.
XBDM_LINK_TEST(XbdmFaultIntegration, HostileAnswers) {
  Rig rig(link);
  ut::TempDir dir;
  REQUIRE(ut::writeFile(dir.file("up"), ut::patternBytes(700, 3)));
  auto client = rig.client(impatient());
  const int seeds = link == Link::Memory ? 240 : 60;
  int failures = 0;
  for (int seed = 0; seed < seeds; ++seed) {
    const int which = seed % 12;
    static const char *commands[] = {"dbgname", "drivelist", "drivefreespace", "dirlist", "getfileattributes",
                                     "getfile", "sendfile", "getmem", "getmemex", "screenshot", "modules", "walkmem"};
    rig.mock.inject(XbdmFault::hostile(static_cast<uint64_t>(seed) * 7919u + 1).on(commands[which]));
    updclient::Result<void> r;
    switch (which) {
    case 0: r = asVoid(client.debugName()); break;
    case 1: r = asVoid(client.drives()); break;
    case 2: r = asVoid(client.driveSpace("HDD")); break;
    case 3: r = asVoid(client.list("HDD:\\")); break;
    case 4: r = asVoid(client.attributes("HDD:\\default.xex")); break;
    case 5: r = client.downloadToFile("HDD:\\Games\\Mock\\default.xex", dir.file("down")); break;
    case 6: r = client.uploadFromFile(dir.file("up"), "HDD:\\up.bin"); break;
    case 7: r = asVoid(client.getMemory(0x82000000u, 0x80)); break;
    case 8: r = asVoid(client.getMemoryEx(0x82000000u, 0x900)); break;
    case 9: r = asVoid(client.screenshot()); break;
    case 10: r = asVoid(client.modules()); break;
    default: r = asVoid(client.memoryRegions()); break;
    }
    if (!r) ++failures;
    rig.mock.clearFaults();
    if (!client.isConnected() || !client.debugName()) {
      if (client.transferActive()) continue;
      REQUIRE_OK(client.reconnect());
    }
  }
  CHECK(failures > seeds / 4);
  REQUIRE_OK(client.reconnect());
  CHECK_OK(client.debugName());
  for (const auto &name : rig.mock.listNames("HDD:\\").value_or(std::vector<std::string>{})) {
    CHECK_MSG(name.find(".part") == std::string::npos || !client.pendingCleanup().empty(), name);
  }
}
