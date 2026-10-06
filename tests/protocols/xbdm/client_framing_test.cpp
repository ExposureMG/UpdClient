#include "protocols/xbdm/client_fake.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <updclient/protocols/xbdm/client.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace updclient;
using namespace updclient::xbdm;
using xt::FakeConsole;

namespace {

XbdmClient connected(const std::shared_ptr<FakeConsole> &console, ClientOptions options = xt::quickOptions()) {
  auto client = xt::attach(console, options);
  if (!client) throw std::runtime_error("attach failed: " + formatError(client.error()));
  return std::move(*client);
}

ut::Bytes le32(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
          static_cast<uint8_t>(v >> 24)};
}

// getfile answers: 203, the 4-byte length, then the given bytes.
void serveFile(FakeConsole &c, uint32_t announced, const ut::Bytes &data) {
  c.line("203- binary response follows");
  c.send(le32(announced));
  c.send(data);
}

Result<ut::Bytes> readAll(FileReader &reader, size_t piece = 7) {
  ut::Bytes out;
  std::vector<uint8_t> buffer(piece);
  for (;;) {
    auto n = reader.read(buffer);
    if (!n) return unexpected<Error>(n.error());
    if (*n == 0) return out;
    out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*n));
  }
}

} // namespace

TEST(XbdmFraming, EveryAnswerShapeSurvivesOneByteReads) {
  const ut::Bytes file = ut::patternBytes(300, 9);
  const ut::Bytes block = ut::patternBytes(20, 4);
  auto console = FakeConsole::create();
  console->setMaxRead(1);
  console->handle([&](FakeConsole &c, const std::string &line) {
    if (line == "dbgname") {
      c.line("200- One Byte");
    } else if (line == "drivelist") {
      c.multiline({"drivename=\"HDD\"", "drivename=\"DEVKIT\""});
    } else if (line == "dirlist name=\"HDD:\\\"") {
      c.multiline({"name=\"a b\" sizehi=0x1 sizelo=0x2 directory"});
    } else if (line == "getfile name=\"HDD:\\f\"") {
      serveFile(c, 300, file);
    } else if (line == "getmemex addr=0x10 length=0x14") {
      c.line("203- binary response follows");
      c.send(ut::Bytes{0x14, 0x80});
      c.send(block);
    } else if (line == "screenshot") {
      c.line("203- binary response follows");
      c.line("pitch=0x4 width=0x1 height=0x1 format=0x0 framebuffersize=0x4");
      c.send(ut::Bytes{1, 2, 3, 4});
    }
  });
  auto client = connected(console);
  CHECK_EQ(client.debugName().value_or(""), std::string("One Byte"));
  CHECK_EQ(client.drives().value_or(std::vector<std::string>{}).size(), size_t{2});
  auto listing = client.list("HDD:\\");
  REQUIRE_OK(listing);
  REQUIRE_EQ(listing->entries.size(), size_t{1});
  CHECK_EQ(listing->entries[0].size, uint64_t{0x100000002});
  auto reader = client.openRead("HDD:\\f");
  REQUIRE_OK(reader);
  auto data = readAll(*reader);
  REQUIRE_OK(data);
  CHECK_EQ(*data, file);
  auto memory = client.getMemoryEx(0x10, 20);
  REQUIRE_OK(memory);
  CHECK_EQ(memory->data, block);
  auto shot = client.screenshot();
  REQUIRE_OK(shot);
  CHECK_EQ(shot->data, (ut::Bytes{1, 2, 3, 4}));
  CHECK(client.isConnected());
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmFraming, BareLineFeedsAndOneCarriageReturnAreStripped) {
  auto console = FakeConsole::create(false);
  console->send(std::string_view("201- connected\n"));
  console->on("dbgname", "200- name\r\r\n").on("drivelist", "202- x\ndrivename=\"HDD\"\n.\n");
  auto client = connected(console);
  CHECK_EQ(client.debugName().value_or(""), std::string("name\r"));
  CHECK_EQ(client.drives().value_or(std::vector<std::string>{}), std::vector<std::string>{"HDD"});
}

TEST(XbdmFraming, LinesAboveTheLimitDropTheConnection) {
  ClientOptions options = xt::quickOptions();
  options.maxLineBytes = 64;
  {
    auto console = FakeConsole::create();
    console->on("dbgname", "200- " + std::string(59, 'x') + "\r\n");
    auto client = connected(console, options);
    CHECK_EQ(client.debugName().value_or("").size(), size_t{59});
  }
  {
    auto console = FakeConsole::create();
    console->on("dbgname", "200- " + std::string(60, 'x') + "\r\n");
    auto client = connected(console, options);
    CHECK_ERR(client.debugName(), ErrorCode::LimitExceeded);
    CHECK(!client.isConnected());
  }
  {
    // A line that never ends is refused once the limit is passed, not at the end.
    auto console = FakeConsole::create();
    console->on("dbgname", "200- " + std::string(100000, 'y'));
    auto client = connected(console, options);
    CHECK_ERR(client.debugName(), ErrorCode::LimitExceeded);
    CHECK(console->unread() > 0);
  }
  {
    auto console = FakeConsole::create();
    console->on("drivelist", "202- x\r\n" + std::string(200, 'z') + "\r\n.\r\n");
    auto client = connected(console, options);
    CHECK_ERR(client.drives(), ErrorCode::LimitExceeded);
  }
}

TEST(XbdmFraming, BodiesAboveTheLimitDropTheConnection) {
  ClientOptions options = xt::quickOptions();
  options.maxBodyBytes = 100;
  auto console = FakeConsole::create();
  std::string body = "202- multiline response follows\r\n";
  for (int i = 0; i < 1000; ++i) body += "drivename=\"D" + std::to_string(i) + "\"\r\n";
  console->on("drivelist", body);
  auto client = connected(console, options);
  CHECK_ERR(client.drives(), ErrorCode::LimitExceeded);
  CHECK(!client.isConnected());
}

TEST(XbdmFraming, BogusDownloadLengthsAreRefusedBeforeTheBody) {
  {
    auto console = FakeConsole::create();
    console->handle([](FakeConsole &c, const std::string &) { serveFile(c, 0x1000, ut::Bytes(16, 1)); });
    auto client = connected(console);
    auto r = client.openRead("HDD:\\f", 16);
    REQUIRE_ERR(r, ErrorCode::LimitExceeded);
    CHECK(!client.isConnected());
    CHECK(!client.transferActive());
  }
  {
    ClientOptions options = xt::quickOptions();
    options.maxDownloadBytes = 1024;
    auto console = FakeConsole::create();
    console->handle([](FakeConsole &c, const std::string &) { serveFile(c, 0xFFFFFFFF, {}); });
    auto client = connected(console, options);
    CHECK_ERR(client.openRead("HDD:\\f"), ErrorCode::LimitExceeded);
  }
  {
    // 4 GiB - 1 announced and then silence: no allocation, the idle timeout ends it.
    auto console = FakeConsole::create();
    console->handle([](FakeConsole &c, const std::string &) { serveFile(c, 0xFFFFFFFF, ut::Bytes(10, 2)); });
    auto client = connected(console);
    auto reader = client.openRead("HDD:\\f");
    REQUIRE_OK(reader);
    CHECK_EQ(reader->size(), uint64_t{0xFFFFFFFF});
    auto data = readAll(*reader, 4096);
    REQUIRE_ERR(data, ErrorCode::Timeout);
    CHECK_EQ(reader->position(), uint64_t{10});
    CHECK(!reader->isOpen());
    CHECK(!client.isConnected());
  }
  {
    auto console = FakeConsole::create();
    console->handle([](FakeConsole &c, const std::string &) {
      c.line("203- binary response follows");
      c.send(ut::Bytes{1, 2});
      c.hangUp();
    });
    auto client = connected(console);
    CHECK_ERR(client.openRead("HDD:\\f"), ErrorCode::Disconnected);
  }
}

TEST(XbdmFraming, TruncatedBinaryData) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &) {
    serveFile(c, 100, ut::Bytes(40, 3));
    c.hangUp();
  });
  auto client = connected(console);
  auto reader = client.openRead("HDD:\\f");
  REQUIRE_OK(reader);
  auto data = readAll(*reader);
  REQUIRE_ERR(data, ErrorCode::Disconnected);
  CHECK_EQ(reader->position(), uint64_t{40});
  CHECK(!client.isConnected());
  CHECK(!client.transferActive());

  auto memory = FakeConsole::create();
  memory->handle([](FakeConsole &c, const std::string &) {
    c.line("203- binary response follows");
    c.send(ut::Bytes{0x10, 0x80, 1, 2, 3});
    c.hangUp();
  });
  auto other = connected(memory);
  CHECK_ERR(other.getMemoryEx(0, 16), ErrorCode::Disconnected);

  auto shot = FakeConsole::create();
  shot->handle([](FakeConsole &c, const std::string &) {
    c.line("203- binary response follows");
    c.line("pitch=0x4 width=0x1 height=0x1 format=0x0 framebuffersize=0x4");
    c.send(ut::Bytes{1});
    c.hangUp();
  });
  auto third = connected(shot);
  CHECK_ERR(third.screenshot(), ErrorCode::Disconnected);
}

TEST(XbdmFraming, ExtraBytesAfterAnAnswerAreCaughtAtTheNextCommand) {
  {
    auto console = FakeConsole::create();
    console->handle([](FakeConsole &c, const std::string &line) {
      if (line == "getfile name=\"HDD:\\f\"") serveFile(c, 4, ut::Bytes{1, 2, 3, 4, 5, 6, 7});
      else c.line("200- OK");
    });
    auto client = connected(console);
    auto reader = client.openRead("HDD:\\f");
    REQUIRE_OK(reader);
    auto data = readAll(*reader);
    REQUIRE_OK(data);
    CHECK_EQ(*data, (ut::Bytes{1, 2, 3, 4}));
    auto next = client.debugName();
    REQUIRE_ERR(next, ErrorCode::Protocol);
    CHECK(next.error().message.find("3 bytes") != std::string::npos);
    CHECK(!client.isConnected());
    CHECK_EQ(console->commands().size(), size_t{1});
  }
  {
    auto console = FakeConsole::create();
    console->on("dvdeject", "200- OK\r\n200- OK\r\n");
    auto client = connected(console);
    CHECK_OK(client.ejectTray());
    CHECK_ERR(client.ejectTray(), ErrorCode::Protocol);
  }
  {
    auto console = FakeConsole::create();
    console->on("drivelist", "202- x\r\ndrivename=\"HDD\"\r\n.\r\nextra\r\n");
    auto client = connected(console);
    CHECK_OK(client.drives());
    CHECK_ERR(client.drives(), ErrorCode::Protocol);
  }
}

TEST(XbdmFraming, SilenceTripsTheIdleTimeout) {
  auto silent = FakeConsole::create();
  silent->on("dbgname", "");
  auto client = connected(silent);
  auto r = client.debugName();
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(r.error().message.find("stopped responding") != std::string::npos);
  CHECK(!client.isConnected());
  CHECK(silent->closed());

  auto midBody = FakeConsole::create();
  midBody->on("drivelist", "202- x\r\ndrivename=\"HDD\"\r\n");
  auto second = connected(midBody);
  CHECK_ERR(second.drives(), ErrorCode::Timeout);

  auto midLine = FakeConsole::create();
  midLine->on("dbgname", "200- half");
  auto third = connected(midLine);
  CHECK_ERR(third.debugName(), ErrorCode::Timeout);

  auto midStatus = FakeConsole::create();
  midStatus->on("dbgname", "20");
  auto fourth = connected(midStatus);
  CHECK_ERR(fourth.debugName(), ErrorCode::Timeout);
}

TEST(XbdmFraming, TimeoutsAreTheConfiguredOnes) {
  ClientOptions options = xt::quickOptions();
  options.greetingTimeout = std::chrono::milliseconds(111);
  options.idleTimeout = std::chrono::milliseconds(222);
  options.slowIdleTimeout = std::chrono::milliseconds(333);
  options.commandTimeout = std::chrono::milliseconds(0);
  auto console = FakeConsole::create();
  std::vector<std::chrono::milliseconds> seen;
  console->handle([&](FakeConsole &c, const std::string &line) {
    seen.push_back(c.lastTimeout());
    if (line == "dbgname") c.line("200- x");
    if (line == "magicboot") c.line("200- OK");
  });
  auto client = connected(console, options);
  CHECK_EQ(console->lastTimeout(), std::chrono::milliseconds(111));
  CHECK_OK(client.debugName());
  CHECK_EQ(console->lastTimeout(), std::chrono::milliseconds(222));
  CHECK_OK(client.reboot());
  CHECK_EQ(console->lastTimeout(), std::chrono::milliseconds(333));
  REQUIRE_EQ(seen.size(), size_t{2});
  CHECK_EQ(seen[0], std::chrono::milliseconds(222));

  ClientOptions capped = xt::quickOptions();
  capped.idleTimeout = std::chrono::milliseconds(0);
  capped.commandTimeout = std::chrono::milliseconds(5000);
  auto other = FakeConsole::create();
  other->on("dbgname", "200- y\r\n");
  auto second = connected(other, capped);
  CHECK_OK(second.debugName());
  CHECK(other->lastTimeout() > std::chrono::milliseconds(0));
  CHECK(other->lastTimeout() <= std::chrono::milliseconds(5000));
}

TEST(XbdmFraming, ATransferOwnsTheConnection) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &line) {
    if (line == "getfile name=\"HDD:\\f\"") serveFile(c, 10, ut::Bytes(10, 5));
    else c.line("200- OK");
  });
  auto client = connected(console);
  auto reader = client.openRead("HDD:\\f");
  REQUIRE_OK(reader);
  CHECK(client.transferActive());
  CHECK_ERR(client.debugName(), ErrorCode::InvalidArgument);
  CHECK_ERR(client.openRead("HDD:\\f"), ErrorCode::InvalidArgument);
  CHECK_ERR(client.openWrite("HDD:\\g", 1), ErrorCode::InvalidArgument);
  CHECK_ERR(client.reconnect(), ErrorCode::InvalidArgument);
  CHECK_EQ(console->commands().size(), size_t{1});
  auto data = readAll(*reader);
  REQUIRE_OK(data);
  CHECK(!client.transferActive());
  CHECK_OK(client.ejectTray());
  CHECK_EQ(console->problems(), std::string());
}

TEST(XbdmFraming, ReaderOutlivesItsClient) {
  const ut::Bytes file = ut::patternBytes(50, 6);
  auto console = FakeConsole::create();
  console->handle([&](FakeConsole &c, const std::string &) { serveFile(c, 50, file); });
  std::optional<FileReader> reader;
  {
    auto client = connected(console);
    auto opened = client.openRead("HDD:\\f");
    REQUIRE_OK(opened);
    reader.emplace(std::move(*opened));
  }
  auto data = readAll(*reader);
  REQUIRE_OK(data);
  CHECK_EQ(*data, file);
  CHECK_EQ(console->byes(), 0);
  CHECK(!console->closed());
  reader.reset();
  CHECK_EQ(console->byes(), 1);
  CHECK(console->closed());
}

TEST(XbdmFraming, ALongLineArrivingByteByByteIsReadInLinearTime) {
  auto console = FakeConsole::create();
  console->setMaxRead(1);
  console->on("dbgname", "200- " + std::string(60000, 'n') + "\r\n");
  auto client = connected(console);
  const auto start = std::chrono::steady_clock::now();
  auto name = client.debugName();
  REQUIRE_OK(name);
  CHECK_EQ(name->size(), size_t{60000});
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}
