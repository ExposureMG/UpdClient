#include "support/test_harness.hpp"
#include "support/xbdm_mock_server.hpp"

#include <updclient/net/tcp_transport.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <thread>

// The mock on its own, spoken to with raw bytes: every obligation of section 5.1
// that the client cannot reach (it never pipelines, never sends an empty line or
// an unknown command), and the shapes the client tests rely on.

using namespace std::chrono_literals;
using updclient::ErrorCode;
using ut::Bytes;
using ut::XbdmFault;
using ut::XbdmMockOptions;
using ut::XbdmMockServer;

namespace {

class Raw {
public:
  explicit Raw(XbdmMockServer &mock) {
    auto t = mock.connect();
    if (t) {
      transport_ = std::move(*t);
      (void)transport_->setTimeout(5000ms);
    }
  }
  explicit Raw(updclient::net::TransportPtr transport) : transport_(std::move(transport)) {}

  bool ok() const { return transport_ != nullptr; }

  void send(std::string_view text) {
    (void)transport_->writeAll(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
  }

  // One line without CR LF; "<eof>" at end of stream, "<error>" on a failure.
  std::string line() {
    while (true) {
      const size_t lf = buffer_.find('\n');
      if (lf != std::string::npos) {
        std::string out = buffer_.substr(0, lf);
        buffer_.erase(0, lf + 1);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return out;
      }
      if (!fill()) return eof_ ? "<eof>" : "<error>";
    }
  }

  Bytes bytes(size_t count) {
    while (buffer_.size() < count) {
      if (!fill()) break;
    }
    const size_t n = std::min(count, buffer_.size());
    Bytes out(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(n));
    buffer_.erase(0, n);
    return out;
  }

  // True when the peer closed without sending anything more.
  bool closed() {
    if (!buffer_.empty()) return false;
    return !fill() && eof_;
  }

  std::string raw() const { return buffer_; }

private:
  bool fill() {
    uint8_t chunk[4096];
    auto n = transport_->readSome(chunk);
    if (!n) return false;
    if (*n == 0) {
      eof_ = true;
      return false;
    }
    buffer_.append(reinterpret_cast<const char *>(chunk), *n);
    return true;
  }

  updclient::net::TransportPtr transport_;
  std::string buffer_;
  bool eof_ = false;
};

std::vector<std::string> body(Raw &raw) {
  std::vector<std::string> lines;
  for (std::string l = raw.line(); l != "." && l != "<eof>" && l != "<error>"; l = raw.line()) lines.push_back(l);
  return lines;
}

uint32_t le32(const Bytes &b) {
  return uint32_t{b[0]} | (uint32_t{b[1]} << 8) | (uint32_t{b[2]} << 16) | (uint32_t{b[3]} << 24);
}

} // namespace

TEST(XbdmMock, GreetsAndKeepsBytesSentBeforeTheGreeting) {
  XbdmMockServer mock;
  Raw raw(mock);
  REQUIRE(raw.ok());
  raw.send("dbgname\r\n");
  CHECK_EQ(raw.line(), std::string("201- connected"));
  CHECK_EQ(raw.line(), std::string("200- MockDevkit"));
}

TEST(XbdmMock, SplitCommandsBareLfAndCase) {
  XbdmMockServer mock;
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("dbg");
  raw.send("na");
  raw.send("me\r");
  raw.send("\n");
  CHECK_EQ(raw.line(), std::string("200- MockDevkit"));
  raw.send("CONSOLETYPE\n");
  CHECK_EQ(raw.line(), std::string("200- devkit"));
  raw.send("getexecstate   \r\n");
  CHECK_EQ(raw.line(), std::string("200- start"));
}

TEST(XbdmMock, TwoCommandsInOneSegmentAreAnsweredInOrderAndRecorded) {
  XbdmMockServer mock;
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("dbgname\r\nconsoletype\r\n");
  CHECK_EQ(raw.line(), std::string("200- MockDevkit"));
  CHECK_EQ(raw.line(), std::string("200- devkit"));
  const auto records = mock.commands();
  REQUIRE_EQ(records.size(), size_t{2});
  CHECK(records[0].pipelined);
  CHECK(!records[1].pipelined);
}

TEST(XbdmMock, PipeliningCanCloseTheConnection) {
  XbdmMockOptions options;
  options.closeOnPipelining = true;
  XbdmMockServer mock(options);
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("dbgname\r\nbye\r\n");
  CHECK(raw.closed());
}

TEST(XbdmMock, UnknownAndEmptyCommandsAre407) {
  XbdmMockServer mock;
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("frobnicate now\r\n");
  CHECK_EQ(raw.line().substr(0, 4), std::string("407-"));
  raw.send("\r\n");
  CHECK_EQ(raw.line().substr(0, 4), std::string("407-"));
  raw.send("whomadethis\r\n");
  CHECK_EQ(raw.line().substr(0, 4), std::string("407-"));
}

TEST(XbdmMock, OverLongLineIs406AndTheConnectionStays) {
  XbdmMockServer mock;
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("dirlist name=\"" + std::string(600, 'a') + "\"\r\n");
  CHECK_EQ(raw.line(), std::string("406- line too long"));
  raw.send("dbgname\r\n");
  CHECK_EQ(raw.line(), std::string("200- MockDevkit"));
  const auto records = mock.commands();
  REQUIRE_EQ(records.size(), size_t{2});
  CHECK(records[0].overLong);
  CHECK_EQ(records[0].line.size(), size_t{512});
}

TEST(XbdmMock, ArgumentsInAnyOrderAndQuotedValuesEndAtTheNextQuote) {
  XbdmMockServer mock;
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("setmem data=\"0102\" ADDR=0x82000000 \r\n");
  CHECK_EQ(raw.line(), std::string("200- set 2 bytes"));
  CHECK_EQ(*mock.memory(0x82000000u, 2), (Bytes{1, 2}));
  raw.send("mkdir name=\"HDD:\\with space\"\r\n");
  CHECK_EQ(raw.line(), std::string("200- OK"));
  CHECK(mock.entry("HDD:\\with space").has_value());
}

TEST(XbdmMock, ListingsAndAttributes) {
  XbdmMockServer mock;
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("drivelist\r\n");
  CHECK_EQ(raw.line(), std::string("202- multiline response follows"));
  CHECK_EQ(body(raw), (std::vector<std::string>{"drivename=\"HDD\"", "drivename=\"DEVKIT\"", "drivename=\"FLASH\"",
                                                 "drivename=\"USB0\""}));
  raw.send("dirlist name=\"HDD:\\Empty\\\"\r\n");
  CHECK_EQ(raw.line(), std::string("202- multiline response follows"));
  CHECK(body(raw).empty());
  raw.send("dirlist name=\"HDD:\\Attrs\"\r\n");
  CHECK_EQ(raw.line(), std::string("202- multiline response follows"));
  const auto entries = body(raw);
  REQUIRE_EQ(entries.size(), size_t{2});
  CHECK(entries[0].find("name=\"ro.txt\" sizehi=0x00000000 sizelo=0x00000009") == 0);
  CHECK(entries[0].find(" readonly") != std::string::npos);
  raw.send("getfileattributes name=\"HDD:\\Content\"\r\n");
  CHECK_EQ(raw.line(), std::string("202- multiline response follows"));
  const auto attrs = body(raw);
  REQUIRE_EQ(attrs.size(), size_t{1});
  CHECK(attrs[0].find(" directory") != std::string::npos);
  raw.send("drivefreespace name=\"HDD:\\\"\r\n");
  CHECK_EQ(raw.line(), std::string("202- multiline response follows"));
  CHECK_EQ(body(raw), (std::vector<std::string>{
                          "freetocallerhi=0x00000017 freetocallerlo=0x4876e800 totalbyteshi=0x0000003a totalbyteslo="
                          "0x35294400 totalfreebyteshi=0x00000017 totalfreebyteslo=0x4876e800"}));
}

TEST(XbdmMock, ErrorsOfSection51) {
  XbdmMockServer mock;
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  const std::pair<const char *, const char *> cases[] = {
      {"mkdir name=\"HDD:\\Content\"", "410-"},
      {"delete name=\"HDD:\\Content\"", "414-"},
      {"delete name=\"HDD:\\Content\" dir", "411-"},
      {"getfile name=\"HDD:\\Content\"", "414-"},
      {"sendfile name=\"HDD:\\Content\" length=0x1", "414-"},
      {"setmem addr=0x10000000 data=00", "404-"},
      {"threadinfo thread=0x1234", "405-"},
      {"go", "408-"},
      {"getfile name=\"HDD:\\missing.bin\"", "402-"},
      {"dirlist name=\"HDD:\\nothing\\\"", "402-"},
      {"dirlist name=\"HDD:\\Protected\\\"", "414- access denied"},
      {"sendfile name=\"HDD:\\no\\such\\folder.bin\" length=0x1", "413-"},
      {"rename name=\"HDD:\\default.xex\" newname=\"HDD:\\Content\"", "410-"},
      {"rename name=\"HDD:\\default.xex\" newname=\"DEVKIT:\\default.xex\"", "409-"},
      {"mkdir name=\"FLASH:\\x\"", "414-"},
  };
  for (const auto &[command, answer] : cases) {
    raw.send(std::string(command) + "\r\n");
    const std::string got = raw.line();
    CHECK_MSG(got.rfind(answer, 0) == 0, std::string(command) + " -> " + got);
  }
  raw.send("stop\r\n");
  CHECK_EQ(raw.line(), std::string("200- OK"));
  raw.send("stop\r\n");
  CHECK_EQ(raw.line().substr(0, 4), std::string("426-"));
}

TEST(XbdmMock, GetfileHasALittleEndianLengthAndNoTrailer) {
  XbdmMockServer mock;
  REQUIRE_OK(mock.addFile("HDD:\\f.bin", ut::patternBytes(300, 3)));
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("getfile name=\"HDD:\\f.bin\"\r\n");
  CHECK_EQ(raw.line(), std::string("203- binary response follows"));
  CHECK_EQ(le32(raw.bytes(4)), 300u);
  CHECK_EQ(raw.bytes(300), ut::patternBytes(300, 3));
  raw.send("dbgname\r\n");
  CHECK_EQ(raw.line(), std::string("200- MockDevkit"));
}

TEST(XbdmMock, HugeGetfileIsTruncatedOrRefused) {
  XbdmMockServer mock;
  REQUIRE_OK(mock.addVirtualFile("HDD:\\huge.bin", (5ull << 30), 1));
  {
    Raw raw(mock);
    CHECK_EQ(raw.line(), std::string("201- connected"));
    raw.send("getfile name=\"HDD:\\huge.bin\"\r\n");
    CHECK_EQ(raw.line(), std::string("203- binary response follows"));
    CHECK_EQ(le32(raw.bytes(4)), 1u << 30);
  }
  auto options = mock.options();
  options.truncateHugeGetfile = false;
  mock.setOptions(options);
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("getfile name=\"HDD:\\huge.bin\"\r\n");
  CHECK_EQ(raw.line().substr(0, 4), std::string("414-"));
}

TEST(XbdmMock, GetmemexBlocksAndTheLastBlockBit) {
  XbdmMockServer mock;
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("getmemex addr=0x82000000 length=0x900\r\n");
  CHECK_EQ(raw.line(), std::string("203- binary response follows"));
  const Bytes h1 = raw.bytes(2);
  CHECK_EQ(h1, (Bytes{0x00, 0x04}));
  raw.bytes(0x400);
  CHECK_EQ(raw.bytes(2), (Bytes{0x00, 0x04}));
  raw.bytes(0x400);
  CHECK_EQ(raw.bytes(2), (Bytes{0x00, 0x81}));
  CHECK_EQ(raw.bytes(0x100), *mock.memory(0x82000800u, 0x100));

  raw.send("getmemex addr=0x30000ff0 length=0x20\r\n");
  CHECK_EQ(raw.line(), std::string("203- binary response follows"));
  CHECK_EQ(raw.bytes(2), (Bytes{0x10, 0x80}));
  raw.bytes(0x10);
  raw.send("getmem addr=0x30000ffe length=0x4\r\n");
  CHECK_EQ(raw.line(), std::string("202- multiline response follows"));
  const auto lines = body(raw);
  REQUIRE_EQ(lines.size(), size_t{1});
  CHECK_EQ(lines[0].substr(4), std::string("????"));
}

TEST(XbdmMock, SendfileTakesExactlyTheAnnouncedBytes) {
  XbdmMockServer mock;
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("sendfile name=\"HDD:\\up.bin\" length=0x5\r\n");
  CHECK_EQ(raw.line(), std::string("204- send binary data"));
  raw.send("hello");
  CHECK_EQ(raw.line(), std::string("200- OK"));
  CHECK_EQ(*mock.fileData("HDD:\\up.bin"), ut::bytesOf("hello"));
  const auto uploads = mock.uploads();
  REQUIRE_EQ(uploads.size(), size_t{1});
  CHECK(uploads[0].completed);
}

TEST(XbdmMock, DroppedSendfileKeepsWhatArrived) {
  XbdmMockServer mock;
  mock.inject(XbdmFault::dropUploadAfterBytes(3).on("sendfile"));
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("sendfile name=\"HDD:\\up.bin\" length=0x5\r\n");
  CHECK_EQ(raw.line(), std::string("204- send binary data"));
  raw.send("hel");
  CHECK(raw.closed());
  REQUIRE(mock.waitUntilIdle());
  REQUIRE(mock.waitForActiveConnections(0));
  CHECK_EQ(*mock.fileData("HDD:\\up.bin"), ut::bytesOf("hel"));
}

TEST(XbdmMock, ByeAndPowerCommandsClose) {
  XbdmMockServer mock;
  {
    Raw raw(mock);
    CHECK_EQ(raw.line(), std::string("201- connected"));
    raw.send("bye\r\n");
    CHECK_EQ(raw.line(), std::string("200- bye"));
    CHECK(raw.closed());
  }
  {
    Raw raw(mock);
    CHECK_EQ(raw.line(), std::string("201- connected"));
    raw.send("magicboot COLD\r\n");
    CHECK(raw.closed());
  }
  {
    Raw raw(mock);
    CHECK_EQ(raw.line(), std::string("201- connected"));
    raw.send("magicboot\r\n");
    CHECK_EQ(raw.line(), std::string("200- OK"));
    CHECK(raw.closed());
  }
  CHECK_EQ(mock.events(), (std::vector<std::string>{"magicboot cold", "magicboot"}));
}

TEST(XbdmMock, ConnectionLimitAnswers401AndCloses) {
  XbdmMockOptions options;
  options.connectionLimit = 2;
  XbdmMockServer mock(options);
  Raw a(mock), b(mock), c(mock);
  CHECK_EQ(a.line(), std::string("201- connected"));
  CHECK_EQ(b.line(), std::string("201- connected"));
  CHECK_EQ(c.line(), std::string("401- max number of connections exceeded"));
  CHECK(c.closed());
  CHECK_EQ(mock.connectionsRefused(), size_t{1});
  CHECK_EQ(mock.activeConnections(), size_t{2});
}

TEST(XbdmMock, NotifyDedicatesTheConnection) {
  XbdmMockServer mock;
  mock.setNotificationsBeforeDedicated({"execution stopped"});
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("notify reconnectport=0x3039 reverse\r\n");
  CHECK_EQ(raw.line(), std::string("execution stopped"));
  CHECK_EQ(raw.line(), std::string("205- now a notification channel"));
  for (int i = 0; i < 500 && mock.notify("debugstr thread=0x1 lf string=hi") == 0; ++i) {
    std::this_thread::sleep_for(1ms);
  }
  CHECK_EQ(raw.line(), std::string("debugstr thread=0x1 lf string=hi"));
}

TEST(XbdmMock, FaultsAreTargeted) {
  XbdmMockServer mock;
  mock.inject(XbdmFault::statusLine("299- odd").on("dbgname").after(1));
  Raw raw(mock);
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("dbgname\r\n");
  CHECK_EQ(raw.line(), std::string("200- MockDevkit"));
  raw.send("consoletype\r\n");
  CHECK_EQ(raw.line(), std::string("200- devkit"));
  raw.send("dbgname\r\n");
  CHECK_EQ(raw.line(), std::string("299- odd"));
  CHECK_EQ(mock.pendingFaults(), size_t{0});
  raw.send("dbgname\r\n");
  CHECK_EQ(raw.line(), std::string("200- MockDevkit"));
}

TEST(XbdmMock, NameProtocol) {
  XbdmMockServer mock;
  CHECK_EQ(*mock.answerNameRequest(Bytes{3, 0}), ut::concat({Bytes{2, 10}, ut::bytesOf("MockDevkit")}));
  CHECK(mock.answerNameRequest(ut::concat({Bytes{1, 10}, ut::bytesOf("mockdevkit")})).has_value());
  CHECK(!mock.answerNameRequest(ut::concat({Bytes{1, 5}, ut::bytesOf("Other")})).has_value());
  CHECK(!mock.answerNameRequest(Bytes{7, 0}).has_value());
  mock.setUdpMode(ut::XbdmUdpMode::Silent);
  CHECK(!mock.answerNameRequest(Bytes{3, 0}).has_value());
}

TEST(XbdmMock, ServesOverLoopbackTcp) {
  XbdmMockServer mock;
  auto port = mock.listenTcp();
  if (!port) SKIP("loopback TCP refused: " + updclient::formatError(port.error()));
  auto transport = updclient::net::TcpTransport::connect("127.0.0.1", *port, 5000ms);
  REQUIRE_OK(transport);
  Raw raw(std::move(*transport));
  CHECK_EQ(raw.line(), std::string("201- connected"));
  raw.send("dbgname\r\n");
  CHECK_EQ(raw.line(), std::string("200- MockDevkit"));
  raw.send("bye\r\n");
  CHECK_EQ(raw.line(), std::string("200- bye"));
  CHECK(raw.closed());
}
