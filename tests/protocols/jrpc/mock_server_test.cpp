#include "support/jrpc_mock_server.hpp"
#include "support/test_harness.hpp"

#include <net/tcp_transport.hpp>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <future>
#include <string>
#include <string_view>
#include <thread>

// The mock on its own, spoken to with raw bytes: nothing here builds a command with the
// client's code. The request lines are the document's own examples (JRPC_PROTOCOL.md
// section 7) and hand-written lines in the same shape, so the mock's parser is checked
// against the document and the client tests can then trust it. Raw strings use a custom
// delimiter because the lines hold backslashes and quotes.

using namespace std::chrono_literals;
using updclient::ErrorCode;
using ut::Bytes;
using ut::JrpcArgument;
using ut::JrpcCall;
using ut::JrpcFault;
using ut::JrpcMockOptions;
using ut::JrpcMockServer;
using ut::JrpcReturn;

namespace {

class Raw {
public:
  explicit Raw(JrpcMockServer &mock) {
    auto t = mock.connect();
    if (t) {
      transport_ = std::move(*t);
      (void)transport_->setTimeout(5000ms);
    }
  }
  explicit Raw(updclient::net::TransportPtr transport) : transport_(std::move(transport)) {}
  Raw(const Raw &) = delete;
  Raw &operator=(const Raw &) = delete;
  // Closes like a client going away, which frees the connection's slot on the server.
  ~Raw() {
    if (transport_) transport_->close();
  }

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

  // The raw bytes of the next `count`, CR LF included.
  std::string bytes(size_t count) {
    while (buffer_.size() < count) {
      if (!fill()) break;
    }
    const size_t n = std::min(count, buffer_.size());
    std::string out = buffer_.substr(0, n);
    buffer_.erase(0, n);
    return out;
  }

  // True when the peer closed without sending anything more.
  bool closed() {
    if (!buffer_.empty()) return false;
    return !fill() && eof_;
  }

  // True when nothing arrives within `wait` (and nothing is buffered).
  bool quiet(std::chrono::milliseconds wait = 100ms) {
    if (!buffer_.empty()) return false;
    (void)transport_->setTimeout(wait);
    uint8_t chunk[4096];
    auto n = transport_->readSome(chunk);
    (void)transport_->setTimeout(5000ms);
    if (n && *n > 0) {
      buffer_.append(reinterpret_cast<const char *>(chunk), *n);
      return false;
    }
    return !n && n.error().code == ErrorCode::Timeout;
  }

  // Everything that arrives until the peer closes or `wait` passes without data.
  std::string drain(std::chrono::milliseconds wait = 150ms) {
    (void)transport_->setTimeout(wait);
    while (fill()) {
    }
    (void)transport_->setTimeout(5000ms);
    std::string out = std::move(buffer_);
    buffer_.clear();
    return out;
  }

  void close() { transport_->close(); }

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

// A connection that has read the banner.
class Session : public Raw {
public:
  explicit Session(JrpcMockServer &mock) : Raw(mock) {
    if (ok()) banner = line();
  }
  std::string banner;
};

// A function that returns its first argument as r3 (what a test needs to see how the
// mock read a number).
JrpcReturn echoFirst(const JrpcCall &call) {
  return JrpcReturn::integer(call.args.empty() ? 0 : static_cast<uint64_t>(call.args[0].integer));
}

// A call by address of the given type, with `argc` and `args` already in wire form.
std::string byAddress(int type, std::string_view address, std::string_view argc, std::string_view args = "",
                      int as = 0) {
  return "consolefeatures ver=2 type=" + std::to_string(type) + " as=" + std::to_string(as) + " params=\"A\\" +
         std::string(address) + "\\A\\" + std::string(argc) + "\\" + std::string(args) + "\"\r\n";
}

// An opcode request.
std::string opcode(int type, std::string_view argc = "0", std::string_view args = "") {
  return "consolefeatures ver=2 type=" + std::to_string(type) + " params=\"A\\0\\A\\" + std::string(argc) + "\\" +
         std::string(args) + "\"\r\n";
}

// An address as the server prints it: %X.
std::string format32(uint32_t value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%X", static_cast<unsigned>(value));
  return buffer;
}

constexpr const char *kBadParams = "error=The paramaters were not found";

} // namespace

// --- Banner and framing ---

TEST(JrpcMock, SendsTheBannerOnAccept) {
  JrpcMockServer mock;
  Raw raw(mock);
  REQUIRE(raw.ok());
  CHECK_EQ(raw.bytes(17), std::string("JRPC2 connected\r\n"));
  CHECK(raw.quiet());
}

TEST(JrpcMock, BannerCanBeOmittedOrWrong) {
  {
    JrpcMockOptions options;
    options.sendBanner = false;
    JrpcMockServer mock(options);
    Raw raw(mock);
    CHECK(raw.quiet());
    // The first thing it ever says is the answer to a command.
    raw.send(opcode(13));
    CHECK_EQ(raw.line(), std::string("17559"));
  }
  {
    JrpcMockOptions options;
    options.banner = "XBDM ready";
    JrpcMockServer mock(options);
    Raw raw(mock);
    CHECK_EQ(raw.line(), std::string("XBDM ready"));
  }
}

TEST(JrpcMock, BannerFaultsApplyToTheGreeting) {
  JrpcMockServer mock;
  mock.inject(JrpcFault::dropAfterBytes(4).onGreeting());
  Raw first(mock);
  CHECK_EQ(first.drain(), std::string("JRPC"));
  CHECK(first.closed());
  // The fault fired once; the next connection is greeted properly.
  Session second(mock);
  CHECK_EQ(second.banner, std::string("JRPC2 connected"));
}

TEST(JrpcMock, LineEndingsSplitWritesAndPipelining) {
  JrpcMockServer mock;
  Session s(mock);
  s.send("consolefeatures ver=2 type=13 params=\"A\\0\\A\\0\\\"\n");
  CHECK_EQ(s.line(), std::string("17559"));
  s.send("consolefeatures ver=2 ty");
  s.send("pe=16 params=\"A\\0\\A\\0\\");
  s.send("\"\r");
  s.send("\n");
  CHECK_EQ(s.line(), std::string("FFFE07D1"));
  s.send(opcode(13) + opcode(16) + opcode(13));
  CHECK_EQ(s.line(), std::string("17559"));
  CHECK_EQ(s.line(), std::string("FFFE07D1"));
  CHECK_EQ(s.line(), std::string("17559"));
  const auto records = mock.commands();
  REQUIRE_EQ(records.size(), size_t{5});
  CHECK(!records[0].pipelined);
  CHECK(records[2].pipelined);
  CHECK(records[3].pipelined);
  CHECK(!records[4].pipelined);
  CHECK_EQ(records[0].line, std::string("consolefeatures ver=2 type=13 params=\"A\\0\\A\\0\\\""));
  CHECK_EQ(records[0].name, std::string("kernel"));
  CHECK_EQ(records[0].type, std::optional<int>(13));
}

TEST(JrpcMock, EmptyLinesAreIgnoredAndByeCloses) {
  JrpcMockServer mock;
  Session s(mock);
  s.send("\r\n\n  \r\n");
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("17559"));
  CHECK_EQ(mock.activeConnections(), size_t{1});
  s.send("Bye\r\n");
  CHECK(s.closed());
  REQUIRE(mock.waitForActiveConnections(0));
  const auto records = mock.commands();
  REQUIRE_EQ(records.size(), size_t{5});
  CHECK_EQ(records[0].name, std::string("empty"));
  CHECK_EQ(records[4].name, std::string("bye"));
}

TEST(JrpcMock, ByeInTheMiddleOfAPipelineStopsServing) {
  JrpcMockServer mock;
  Session s(mock);
  s.send(opcode(13) + "Bye\r\n" + opcode(16));
  CHECK_EQ(s.line(), std::string("17559"));
  CHECK(s.closed());
}

// --- Section 7, byte for byte ---

TEST(JrpcMock, Example71VoidCall) {
  JrpcMockServer mock;
  mock.registerFunction(0x82000000u, [](const JrpcCall &) { return JrpcReturn::integer(0); });
  Session s(mock);
  s.send(R"JR(consolefeatures ver=2 type=0 as=0 params="A\82000000\A\0\")JR"
         "\r\n");
  CHECK_EQ(s.line(), std::string("0"));
  const auto calls = mock.calls();
  REQUIRE_EQ(calls.size(), size_t{1});
  CHECK_EQ(calls[0].type, 0);
  CHECK_EQ(calls[0].address, 0x82000000u);
  CHECK(!calls[0].system);
  CHECK(!calls[0].module.has_value());
  CHECK(calls[0].args.empty());
  CHECK_EQ(mock.callCount(0x82000000u), size_t{1});
}

TEST(JrpcMock, VoidCallAnswersHexOfR3OrSOk) {
  JrpcMockServer mock;
  mock.registerFunction(0x82000000u, [](const JrpcCall &) { return JrpcReturn::integer(0x80070005ull); });
  Session s(mock);
  const std::string call = byAddress(0, "82000000", "0");
  s.send(call);
  CHECK_EQ(s.line(), std::string("80070005"));
  auto options = mock.options();
  options.voidAnswer = ut::JrpcVoidAnswer::SOk;
  mock.setOptions(options);
  s.send(call);
  CHECK_EQ(s.line(), std::string("S_OK"));
}

TEST(JrpcMock, Example72IntCallWithTwoArguments) {
  JrpcMockServer mock;
  mock.registerFunction(0x82010000u, [](const JrpcCall &call) {
    return JrpcReturn::integer(call.args.at(0).integer * call.args.at(1).integer / 2 + 2);
  });
  Session s(mock);
  s.send(R"JR(consolefeatures ver=2 type=1 as=0 params="A\82010000\A\2\1\5\1\16\")JR"
         "\r\n");
  CHECK_EQ(s.line(), std::string("2A"));
  const auto calls = mock.calls();
  REQUIRE_EQ(calls.size(), size_t{1});
  REQUIRE_EQ(calls[0].args.size(), size_t{2});
  CHECK_EQ(calls[0].args[0].tag, '1');
  CHECK_EQ(calls[0].args[0].integer, int64_t{5});
  CHECK_EQ(calls[0].args[1].integer, int64_t{16});
}

TEST(JrpcMock, Example73ByOrdinalOnASystemThreadWithAn64BitReturn) {
  JrpcMockServer mock;
  const uint32_t address = mock.registerFunction("xam.xex", 436, [](const JrpcCall &call) {
    CHECK(call.system);
    return JrpcReturn::integer(0x0000000248173A00ull);
  });
  Session s(mock);
  s.send(R"JR(consolefeatures ver=2 type=8 system module="xam.xex" ord=436 as=0 params="A\0\A\0\")JR"
         "\r\n");
  // %llX does not pad: the document's example shows 16 digits, the format gives 9.
  CHECK_EQ(s.line(), std::string("248173A00"));
  const auto calls = mock.calls();
  REQUIRE_EQ(calls.size(), size_t{1});
  CHECK(calls[0].system);
  CHECK_EQ(calls[0].module, std::optional<std::string>("xam.xex"));
  CHECK_EQ(calls[0].ordinal, 436u);
  CHECK_EQ(calls[0].address, address);
}

TEST(JrpcMock, Example74StringArgumentIsHexDecoded) {
  JrpcMockServer mock;
  mock.registerFunction(0x82020000u, [](const JrpcCall &) { return JrpcReturn::integer(0); });
  Session s(mock);
  s.send(R"JR(consolefeatures ver=2 type=0 as=0 params="A\82020000\A\1\2/2\6869\")JR"
         "\r\n");
  CHECK_EQ(s.line(), std::string("0"));
  const auto calls = mock.calls();
  REQUIRE_EQ(calls.size(), size_t{1});
  REQUIRE_EQ(calls[0].args.size(), size_t{1});
  CHECK_EQ(calls[0].args[0].tag, '2');
  CHECK_EQ(calls[0].args[0].text(), std::string("hi"));
}

TEST(JrpcMock, Example75XNotifyHasNoReplyBody) {
  JrpcMockServer mock;
  Session s(mock);
  s.send(R"JR(consolefeatures ver=2 type=12 params="A\0\A\2\2/5\48656C6C6F\1\0\")JR"
         "\r\n");
  // Nothing comes back for the notify: the next line read is the next command's answer.
  CHECK(s.quiet());
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("17559"));
  const auto notes = mock.notifications();
  REQUIRE_EQ(notes.size(), size_t{1});
  CHECK_EQ(notes[0].text, std::string("Hello"));
  CHECK_EQ(notes[0].type, 0u);
  CHECK_EQ(mock.events(), (std::vector<std::string>{"notify 0 Hello"}));
}

TEST(JrpcMock, Example76CpuKey) {
  JrpcMockServer mock;
  Session s(mock);
  s.send(R"JR(consolefeatures ver=2 type=10 params="A\0\A\0\")JR"
         "\r\n");
  CHECK_EQ(s.line(), std::string("A1B2C3D4E5F60718"));
}

// --- Return formats (section 6) ---

TEST(JrpcMock, ScalarReturnFormats) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, [](const JrpcCall &call) {
    switch (call.type) {
    case 1: return JrpcReturn::integer(0xFFFFFFFF80000000ull);
    case 2: return JrpcReturn::string("Hello world");
    case 3: return JrpcReturn::real(1.5);
    case 4: return JrpcReturn::integer(0x1FFull);
    default: return JrpcReturn::integer(0xFFFFFFFFFFFFFFFFull);
    }
  });
  Session s(mock);
  s.send(byAddress(1, "1000", "0"));
  CHECK_EQ(s.line(), std::string("80000000"));
  s.send(byAddress(2, "1000", "0"));
  CHECK_EQ(s.line(), std::string("Hello world"));
  s.send(byAddress(3, "1000", "0"));
  CHECK_EQ(s.line(), std::string("1.500000"));
  s.send(byAddress(4, "1000", "0"));
  CHECK_EQ(s.line(), std::string("FF"));
  s.send(byAddress(8, "1000", "0"));
  CHECK_EQ(s.line(), std::string("FFFFFFFFFFFFFFFF"));
}

TEST(JrpcMock, FloatsPrintSixDecimals) {
  JrpcMockServer mock;
  double value = 0;
  mock.registerFunction(0x1000u, [&value](const JrpcCall &) { return JrpcReturn::real(value); });
  Session s(mock);
  const std::pair<double, const char *> cases[] = {
      {0.0, "0.000000"},   {-2.25, "-2.250000"},     {0.0000001, "0.000000"},
      {123456.789, "123456.789000"}, {3.4028234663852886e38, "340282346638528859811704183484516925440.000000"}};
  for (const auto &[in, out] : cases) {
    value = in;
    s.send(byAddress(3, "1000", "0"));
    CHECK_EQ(s.line(), std::string(out));
  }
}

TEST(JrpcMock, ByteReturnPaddingIsAnOption) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, [](const JrpcCall &) { return JrpcReturn::integer(0x0A); });
  Session s(mock);
  s.send(byAddress(4, "1000", "0"));
  CHECK_EQ(s.line(), std::string("A"));
  auto options = mock.options();
  options.byteReplyPadded = true;
  mock.setOptions(options);
  s.send(byAddress(4, "1000", "0"));
  CHECK_EQ(s.line(), std::string("0A"));
}

TEST(JrpcMock, ArrayReturnFormats) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, [](const JrpcCall &call) {
    switch (call.type) {
    case 5: return JrpcReturn::intArray({1, -2, 3});
    case 6: return JrpcReturn::floatArray({1.5, -0.25});
    default: return JrpcReturn::byteArray({0x0A, 0xFF, 0x00});
    }
  });
  Session s(mock);
  s.send(byAddress(5, "1000", "0", "", 3));
  CHECK_EQ(s.line(), std::string("1,-2,3;"));
  s.send(byAddress(5, "1000", "0", "", 2));
  CHECK_EQ(s.line(), std::string("1,-2;"));
  s.send(byAddress(6, "1000", "0", "", 2));
  CHECK_EQ(s.line(), std::string("1.500000,-0.250000;"));
  s.send(byAddress(7, "1000", "0", "", 3));
  CHECK_EQ(s.line(), std::string("A,FF,0;"));
  s.send(byAddress(7, "1000", "0", "", 1));
  CHECK_EQ(s.line(), std::string("A;"));
  // as=0: an empty list.
  s.send(byAddress(5, "1000", "0", "", 0));
  CHECK_EQ(s.line(), std::string(";"));
  // Past the end of what the function returned, the console reads whatever follows.
  s.send(byAddress(5, "1000", "0", "", 5));
  CHECK_EQ(s.line(), std::string("1,-2,3,0,0;"));
}

TEST(JrpcMock, ArraysStopAtEightElementsUnlessTheFormatLoops) {
  JrpcMockServer mock;
  std::vector<int32_t> twelve;
  for (int i = 1; i <= 12; ++i) twelve.push_back(i);
  mock.registerFunction(0x1000u, [&](const JrpcCall &) { return JrpcReturn::intArray(twelve); });
  Session s(mock);
  s.send(byAddress(5, "1000", "0", "", 8));
  CHECK_EQ(s.line(), std::string("1,2,3,4,5,6,7,8;"));
  s.send(byAddress(5, "1000", "0", "", 9));
  CHECK_EQ(s.line(), std::string("1,2,3,4,5,6,7,8;"));
  s.send(byAddress(5, "1000", "0", "", 12));
  CHECK_EQ(s.line(), std::string("1,2,3,4,5,6,7,8;"));

  auto options = mock.options();
  options.arrayOverflow = ut::JrpcArrayOverflow::Loop;
  mock.setOptions(options);
  s.send(byAddress(5, "1000", "0", "", 12));
  CHECK_EQ(s.line(), std::string("1,2,3,4,5,6,7,8,9,10,11,12;"));

  // A fault prints a count of its own, whatever `as` says.
  mock.inject(JrpcFault::arrayElements(10).onType(5));
  s.send(byAddress(5, "1000", "0", "", 2));
  CHECK_EQ(s.line(), std::string("1,2,3,4,5,6,7,8,9,10;"));
  mock.inject(JrpcFault::arrayElements(1).onType(5));
  s.send(byAddress(5, "1000", "0", "", 4));
  CHECK_EQ(s.line(), std::string("1;"));
}

TEST(JrpcMock, FunctionsCanTakeOverTheReply) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, [](const JrpcCall &) { return JrpcReturn::failure("it broke"); });
  mock.registerFunction(0x2000u, [](const JrpcCall &) { return JrpcReturn::line("what is this"); });
  mock.registerFunction(0x3000u, [](const JrpcCall &) { return JrpcReturn::silent(); });
  Session s(mock);
  s.send(byAddress(1, "1000", "0"));
  CHECK_EQ(s.line(), std::string("error=it broke"));
  s.send(byAddress(8, "2000", "0"));
  CHECK_EQ(s.line(), std::string("what is this"));
  s.send(byAddress(1, "3000", "0"));
  CHECK(s.quiet());
  // The connection is still usable.
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("17559"));
}

// --- How the request is read (sections 2 and 3) ---

TEST(JrpcMock, NumbersAreReadLikeSscanf) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, echoFirst);
  Session s(mock);
  const std::pair<const char *, const char *> cases[] = {
      {R"(1\16\)", "10"},          // decimal
      {R"(1\0x10\)", "10"},        // %i takes a 0x prefix
      {R"(1\010\)", "8"},          // and a leading zero means octal
      {R"(1\08\)", "0"},           // 8 is not an octal digit: the scan stops after "0"
      {R"(1\12abc\)", "C"},        // whatever follows the digits is ignored
      {R"(1\-5\)", "FFFFFFFB"},    // negative, sign-extended into r3
      {R"(1\ 7\)", "7"},           // leading white space
      {R"(4\255\)", "FF"},         // a byte is an int too
      {R"(1/1\)", "1"},            // the separator does not matter
      {R"(1/0\)", "0"},
  };
  for (const auto &[arg, reply] : cases) {
    s.send(byAddress(1, "1000", "1", arg));
    CHECK_EQ(s.line(), std::string(reply));
  }
  // No digits at all is not a number.
  s.send(byAddress(1, "1000", "1", R"(1\abc\)"));
  CHECK_EQ(s.line(), std::string(kBadParams));
  s.send(byAddress(1, "1000", "1", R"(1\\)"));
  CHECK_EQ(s.line(), std::string(kBadParams));
}

TEST(JrpcMock, IntegersThatDoNotFitClampOrWrap) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, echoFirst);
  mock.registerFunction(0x2000u, [](const JrpcCall &call) {
    return JrpcReturn::integer(static_cast<uint64_t>(call.args.at(0).integer) >> 32);
  });
  Session s(mock);
  // Default: saturate, so a client that sends a uint32 as it is gets caught.
  s.send(byAddress(1, "1000", "1", R"(1\4294967295\)"));
  CHECK_EQ(s.line(), std::string("7FFFFFFF"));
  s.send(byAddress(1, "1000", "1", R"(1\-4294967295\)"));
  CHECK_EQ(s.line(), std::string("80000000"));
  s.send(byAddress(1, "1000", "1", R"(1\-1\)"));
  CHECK_EQ(s.line(), std::string("FFFFFFFF"));
  s.send(byAddress(8, "2000", "1", R"(8\99999999999999999999\)"));
  CHECK_EQ(s.line(), std::string("7FFFFFFF"));
  s.send(byAddress(8, "1000", "1", R"(8\-1\)"));
  CHECK_EQ(s.line(), std::string("FFFFFFFFFFFFFFFF"));
  s.send(byAddress(8, "1000", "1", R"(8\9223372036854775807\)"));
  CHECK_EQ(s.line(), std::string("7FFFFFFFFFFFFFFF"));

  auto options = mock.options();
  options.intOverflow = ut::JrpcIntOverflow::Wrap;
  mock.setOptions(options);
  s.send(byAddress(1, "1000", "1", R"(1\4294967295\)"));
  CHECK_EQ(s.line(), std::string("FFFFFFFF"));
  CHECK_EQ(mock.calls().back().args[0].integer, int64_t{-1});
  s.send(byAddress(1, "1000", "1", R"(1\4294967296\)"));
  CHECK_EQ(s.line(), std::string("0"));
  s.send(byAddress(8, "2000", "1", R"(8\18446744073709551615\)"));
  CHECK_EQ(s.line(), std::string("FFFFFFFF"));
}

TEST(JrpcMock, FloatArgumentsAreReadWithStrtodRules) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, [](const JrpcCall &call) { return JrpcReturn::real(call.args.at(0).real); });
  Session s(mock);
  s.send(byAddress(3, "1000", "1", R"(3\1.5\)"));
  CHECK_EQ(s.line(), std::string("1.500000"));
  s.send(byAddress(3, "1000", "1", R"(3\-0.000125\)"));
  CHECK_EQ(s.line(), std::string("-0.000125"));
  s.send(byAddress(3, "1000", "1", R"(3\1e3\)"));
  CHECK_EQ(s.line(), std::string("1000.000000"));
  s.send(byAddress(3, "1000", "1", R"(3\0.100000001\)"));
  CHECK_EQ(s.line(), std::string("0.100000"));
  s.send(byAddress(3, "1000", "1", R"(3\x\)"));
  CHECK_EQ(s.line(), std::string(kBadParams));
}

TEST(JrpcMock, StringsAndBlobsAreHexDecoded) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, [](const JrpcCall &) { return JrpcReturn::integer(0); });
  Session s(mock);
  // lower-case hex, an empty string, a blob with a NUL, a string with a space.
  s.send(byAddress(0, "1000", "4", R"(2/3\61626a\2/0\\7/3\00FF10\2/2\2041\)"));
  CHECK_EQ(s.line(), std::string("0"));
  const auto calls = mock.calls();
  REQUIRE_EQ(calls.size(), size_t{1});
  REQUIRE_EQ(calls[0].args.size(), size_t{4});
  CHECK_EQ(calls[0].args[0].text(), std::string("abj"));
  CHECK_EQ(calls[0].args[1].data.size(), size_t{0});
  CHECK_EQ(calls[0].args[1].tag, '2');
  CHECK_EQ(calls[0].args[2].tag, '7');
  CHECK_EQ(calls[0].args[2].data, (Bytes{0x00, 0xFF, 0x10}));
  CHECK_EQ(calls[0].args[3].text(), std::string(" A"));
}

TEST(JrpcMock, EveryArgumentTagOfSection3) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, [](const JrpcCall &) { return JrpcReturn::integer(1); });
  Session s(mock);
  s.send(byAddress(1, "1000", "6", R"(1\-7\2/1\41\3\2.5\4\200\7/2\0102\8\-9\)"));
  CHECK_EQ(s.line(), std::string("1"));
  const auto calls = mock.calls();
  REQUIRE_EQ(calls.size(), size_t{1});
  const auto &args = calls[0].args;
  REQUIRE_EQ(args.size(), size_t{6});
  CHECK_EQ(args[0].tag, '1');
  CHECK_EQ(args[0].integer, int64_t{-7});
  CHECK_EQ(args[1].tag, '2');
  CHECK_EQ(args[2].tag, '3');
  CHECK_EQ(args[2].real, 2.5);
  CHECK_EQ(args[3].tag, '4');
  CHECK_EQ(args[3].integer, int64_t{200});
  CHECK_EQ(args[4].tag, '7');
  CHECK_EQ(args[4].data, (Bytes{0x01, 0x02}));
  CHECK_EQ(args[5].tag, '8');
  CHECK_EQ(args[5].integer, int64_t{-9});
}

TEST(JrpcMock, ThirtySevenArgumentsAreTheLimit) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, [](const JrpcCall &call) { return JrpcReturn::integer(call.args.size()); });
  Session s(mock);
  std::string args37, args38;
  for (int i = 0; i < 37; ++i) args37 += R"(1\1\)";
  args38 = args37 + R"(1\1\)";
  s.send(byAddress(1, "1000", "37", args37));
  CHECK_EQ(s.line(), std::string("25"));
  s.send(byAddress(1, "1000", "38", args38));
  CHECK_EQ(s.line(), std::string(kBadParams));
  s.send(byAddress(1, "1000", "0"));
  CHECK_EQ(s.line(), std::string("0"));
}

TEST(JrpcMock, ArgumentsThatAreNotWellFormedAreRefused) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, [](const JrpcCall &) { return JrpcReturn::integer(1); });
  Session s(mock);
  // Each case announces exactly as many arguments as it supplies, so the one fault under test is the only fault.
  // The twin differs from the bad line in that fault alone and must be accepted: the refusal comes from that check.
  struct Case {
    const char *argc;
    const char *bad;
    const char *good;
    const char *why;
  };
  const Case cases[] = {
      {"2", R"(1\5\)", R"(1\5\1\6\)", "one argument where two are announced"},
      {"1", R"(5\0102\)", R"(1\0102\)", "tag 5 never appears on the wire"},
      {"1", R"(6\0102\)", R"(1\0102\)", "nor tag 6"},
      {"1", R"(9\1\)", R"(1\1\)", "nor 9"},
      {"1", R"(2/1\616\)", R"(2/1\61\)", "an odd number of hex digits"},
      {"1", R"(2/2\61\)", R"(2/2\6162\)", "fewer bytes than the count says"},
      {"1", R"(2/1\6162\)", R"(2/1\61\)", "more"},
      {"1", R"(2/1\6G\)", R"(2/1\6F\)", "not hex"},
      {"1", R"(7/-0\\)", R"(7/0\\)", "a negative count (zero, so no size check can catch it)"},
      {"1", R"(7/0\)", R"(7/0\\)", "no payload field"},
      {"1", R"(1:5\)", R"(1\5\)", "a separator that is neither \\ nor /"},
  };
  for (const Case &c : cases) {
    const size_t before = mock.calls().size();
    s.send(byAddress(1, "1000", c.argc, c.bad));
    const std::string refused = s.line();
    CHECK_MSG(refused == kBadParams, std::string(c.why) + ": " + c.bad + " -> " + refused);
    CHECK_EQ(mock.calls().size(), before);

    s.send(byAddress(1, "1000", c.argc, c.good));
    const std::string accepted = s.line();
    CHECK_MSG(accepted == "1", std::string(c.why) + " (twin): " + c.good + " -> " + accepted);
    CHECK_EQ(mock.calls().size(), before + 1);
  }
  // The connection survived all of it.
  s.send(byAddress(1, "1000", "0"));
  CHECK_EQ(s.line(), std::string("1"));
}

TEST(JrpcMock, HeaderFieldsMayComeInAnyOrderAndUnknownOnesAreIgnored) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, [](const JrpcCall &call) { return JrpcReturn::integer(call.arraySize); });
  Session s(mock);
  s.send(R"JR(consolefeatures as=3 extra=1 ver=2 system type=7 params="A\1000\A\0\")JR"
         "\r\n");
  CHECK_EQ(s.line(), std::string("0,0,0;"));
  CHECK_EQ(mock.calls().back().arraySize, 3);
  CHECK(mock.calls().back().system);
  // Case counts.
  s.send(R"JR(consolefeatures ver=2 type=1 params="A\1000\A\0\")JR"
         "\r\n");
  CHECK_EQ(s.line(), std::string("0"));
}

TEST(JrpcMock, MalformedLinesAreAnsweredOrIgnoredPerOptionAndNeverCloseByDefault) {
  JrpcMockServer mock;
  Session s(mock);
  const std::pair<const char *, const char *> cases[] = {
      {"hello", "error=Unknown command"},
      {"consolefeatures2 ver=2 type=13", "error=Unknown command"},
      {"consolefeatures ver=2 type=13", kBadParams},                       // no params
      {R"(consolefeatures ver=2 params="A\0\A\0\")", kBadParams},          // no type
      {R"(consolefeatures ver=1 type=13 params="A\0\A\0\")", "error=Version mismatch"},
      {R"(consolefeatures type=13 params="A\0\A\0\")", "error=Version mismatch"},
      {R"(consolefeatures ver=2 type=20 params="A\0\A\0\")", kBadParams},
      {R"(consolefeatures ver=2 type=-1 params="A\0\A\0\")", kBadParams},
      {R"(consolefeatures ver=2 type=13 params="A\0\A\0\)", kBadParams},    // no closing quote
      {R"(consolefeatures ver=2 type=13 params="B\0\A\0\")", kBadParams},
      {R"(consolefeatures ver=2 type=13 params="A\zz\A\0\")", kBadParams},
      {R"(consolefeatures ver=2 type=13 params="A\0\A\x\")", kBadParams},
      {R"(consolefeatures ver=2 type=13 params="A\0\A\-1\")", kBadParams},
      {R"(consolefeatures ver=2 type=13 module="xam.xex params="A\0\A\0\")", kBadParams},
  };
  for (const auto &[line, answer] : cases) {
    s.send(std::string(line) + "\r\n");
    const std::string got = s.line();
    CHECK_MSG(got == answer, std::string(line) + " -> " + got);
  }
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("17559"));
  for (const auto &record : mock.commands()) {
    if (record.name == "kernel") continue;
    CHECK_EQ(record.name, std::string("invalid"));
  }
}

TEST(JrpcMock, MalformedLinePolicies) {
  JrpcMockOptions options;
  options.onMalformed = ut::JrpcOnFailure::Silent;
  JrpcMockServer mock(options);
  {
    Session s(mock);
    s.send("nonsense\r\n");
    CHECK(s.quiet());
    s.send(opcode(13));
    CHECK_EQ(s.line(), std::string("17559"));
  }
  options.onMalformed = ut::JrpcOnFailure::Close;
  mock.setOptions(options);
  {
    Session s(mock);
    s.send("nonsense\r\n");
    CHECK(s.closed());
  }
}

// --- Entry points ---

TEST(JrpcMock, UnknownTargetsAnswerTheResolveError) {
  JrpcMockServer mock;
  Session s(mock);
  s.send(byAddress(1, "BEEF", "0"));
  CHECK_EQ(s.line(), std::string(R"(error=Could not resolve function address, params = A\BEEF\A\0\, 1)"));
  s.send(R"JR(consolefeatures ver=2 type=0 module="nowhere.xex" ord=5 as=0 params="A\0\A\0\")JR"
         "\r\n");
  CHECK_EQ(s.line(), std::string(R"(error=Could not resolve function address, params = A\0\A\0\, 0)"));
  // A call is still recorded, so a test can see what the client asked for.
  CHECK_EQ(mock.calls().size(), size_t{2});
  CHECK_EQ(mock.calls()[0].address, 0xBEEFu);
}

TEST(JrpcMock, UnknownTargetPolicies) {
  JrpcMockOptions options;
  options.onUnknownTarget = ut::JrpcOnFailure::Silent;
  JrpcMockServer mock(options);
  {
    Session s(mock);
    s.send(byAddress(1, "BEEF", "0"));
    CHECK(s.quiet());
  }
  options.onUnknownTarget = ut::JrpcOnFailure::Close;
  mock.setOptions(options);
  {
    Session s(mock);
    s.send(byAddress(1, "BEEF", "0"));
    CHECK(s.closed());
  }
}

TEST(JrpcMock, ExportsReachTheSameFunctionByOrdinalAndByAddress) {
  JrpcMockServer mock;
  const uint32_t a = mock.registerFunction("Xam.XEX", 1, [](const JrpcCall &) { return JrpcReturn::integer(0xA); });
  const uint32_t b = mock.registerFunction("xam.xex", 2, [](const JrpcCall &) { return JrpcReturn::integer(0xB); });
  CHECK_NE(a, b);
  Session s(mock);
  s.send(R"JR(consolefeatures ver=2 type=1 module="XAM.xex" ord=1 as=0 params="A\0\A\0\")JR"
         "\r\n");
  CHECK_EQ(s.line(), std::string("A"));
  s.send(byAddress(1, format32(b), "0"));
  CHECK_EQ(s.line(), std::string("B"));
  CHECK_EQ(mock.callCount(a), size_t{1});
  CHECK_EQ(mock.callCount(b), size_t{1});
  // The same export again replaces the function and keeps the address.
  CHECK_EQ(mock.registerFunction("xam.xex", 1, [](const JrpcCall &) { return JrpcReturn::integer(0xC); }), a);
  s.send(R"JR(consolefeatures ver=2 type=1 module="xam.xex" ord=1 as=0 params="A\0\A\0\")JR"
         "\r\n");
  CHECK_EQ(s.line(), std::string("C"));
  // An export that points at an address with no function in it is an unknown target.
  mock.registerExport("kernel", 7, 0x5000);
  s.send(R"JR(consolefeatures ver=2 type=1 module="kernel" ord=7 as=0 params="A\0\A\0\")JR"
         "\r\n");
  CHECK_EQ(s.line().substr(0, 10), std::string("error=Coul"));
  mock.unregisterFunction(b);
  s.send(byAddress(1, format32(b), "0"));
  CHECK_EQ(s.line().substr(0, 10), std::string("error=Coul"));
}

// --- The system opcodes (section 4.2) ---

TEST(JrpcMock, ResolveFunction) {
  JrpcMockServer mock;
  const uint32_t address = mock.registerFunction("xam.xex", 0x1B4, [](const JrpcCall &) { return JrpcReturn::integer(0); });
  Session s(mock);
  // Module "xam.xex" is 78616D2E786578, ordinal 436.
  s.send(opcode(9, "2", R"(2/7\78616D2E786578\1\436\)"));
  CHECK_EQ(s.line(), format32(address));
  s.send(opcode(9, "2", R"(2/7\78616D2E786578\1\437\)"));
  CHECK_EQ(s.line().substr(0, 10), std::string("error=Coul"));
  s.send(opcode(9, "1", R"(1\436\)"));
  CHECK_EQ(s.line(), std::string(kBadParams));
  CHECK_EQ(mock.commands()[0].name, std::string("resolve"));
}

TEST(JrpcMock, InformationOpcodes) {
  JrpcMockServer mock;
  Session s(mock);
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("17559"));
  s.send(opcode(16));
  CHECK_EQ(s.line(), std::string("FFFE07D1"));
  s.send(opcode(17));
  CHECK_EQ(s.line(), std::string("Jasper"));
  const char *temps[] = {"32", "2D", "2A", "28"};
  for (int i = 0; i < 4; ++i) {
    s.send(opcode(15, "1", "1\\" + std::to_string(i) + "\\"));
    CHECK_EQ(s.line(), std::string(temps[i]));
  }
  s.send(opcode(15, "1", R"(1\4\)"));
  CHECK_EQ(s.line(), std::string("0"));
  s.send(opcode(15));
  CHECK_EQ(s.line(), std::string(kBadParams));

  ut::JrpcConsoleInfo info;
  info.kernelVersion = -3;
  info.titleId = 0x41560817;
  info.consoleType = "Corona";
  info.temperatures = {0x41, 0x0, 0xFF, 0x1};
  mock.setInfo(info);
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("-3"));
  s.send(opcode(16));
  CHECK_EQ(s.line(), std::string("41560817"));
  s.send(opcode(17));
  CHECK_EQ(s.line(), std::string("Corona"));
  s.send(opcode(15, "1", R"(1\2\)"));
  CHECK_EQ(s.line(), std::string("FF"));
}

TEST(JrpcMock, CpuKeyPaddingAndWidth) {
  JrpcMockServer mock;
  ut::JrpcConsoleInfo info;
  info.cpuKeyHigh = 0x0000A1B2;
  info.cpuKeyLow = 0x00000018;
  mock.setInfo(info);
  Session s(mock);
  s.send(opcode(10));
  CHECK_EQ(s.line(), std::string("0000A1B200000018"));

  auto options = mock.options();
  options.cpuKeyPadded = false;
  mock.setOptions(options);
  s.send(opcode(10));
  // The two halves run together: the reply cannot be split back.
  CHECK_EQ(s.line(), std::string("A1B218"));

  options.cpuKeyPadded = true;
  options.cpuKeyDigits = 16;
  mock.setOptions(options);
  info.cpuKeyHigh = 0x0123456789ABCDEFull;
  info.cpuKeyLow = 0xFEDCBA9876543210ull;
  mock.setInfo(info);
  s.send(opcode(10));
  CHECK_EQ(s.line(), std::string("0123456789ABCDEFFEDCBA9876543210"));
}

TEST(JrpcMock, OpcodesWithoutReplyCanAnswerInstead) {
  const char *requests[] = {"consolefeatures ver=2 type=12 params=\"A\\0\\A\\2\\2/1\\41\\1\\3\\\"\r\n",
                            "consolefeatures ver=2 type=14 params=\"A\\0\\A\\4\\1\\1\\1\\2\\1\\3\\1\\4\\\"\r\n",
                            "consolefeatures ver=2 type=18 params=\"A\\82000000\\A\\5\\1\\1\\1\\0\\1\\0\\1\\0\\1\\0\\\"\r\n"};
  for (auto answer : {ut::JrpcSilentOpcodeAnswer::Silent, ut::JrpcSilentOpcodeAnswer::SOk,
                      ut::JrpcSilentOpcodeAnswer::Zero}) {
    JrpcMockOptions options;
    options.silentOpcodes = answer;
    JrpcMockServer mock(options);
    Session s(mock);
    for (const char *request : requests) {
      s.send(request);
      switch (answer) {
      case ut::JrpcSilentOpcodeAnswer::Silent: CHECK(s.quiet()); break;
      case ut::JrpcSilentOpcodeAnswer::SOk: CHECK_EQ(s.line(), std::string("S_OK")); break;
      case ut::JrpcSilentOpcodeAnswer::Zero: CHECK_EQ(s.line(), std::string("0")); break;
      }
    }
    // Whatever they did, the effects were the same.
    CHECK_EQ(mock.notifications().size(), size_t{1});
    CHECK_EQ(mock.ledWrites().size(), size_t{1});
    CHECK_EQ(mock.memoryTasks().size(), size_t{1});
    s.send(opcode(13));
    CHECK_EQ(s.line(), std::string("17559"));
  }
}

TEST(JrpcMock, SetLedsAndConstantMemorySetAreRecorded) {
  JrpcMockServer mock;
  Session s(mock);
  s.send(opcode(14, "4", R"(1\1\1\-2\1\3\1\0x10\)"));
  CHECK(s.quiet());
  const auto leds = mock.ledWrites();
  REQUIRE_EQ(leds.size(), size_t{1});
  CHECK_EQ(leds[0].topLeft, 1);
  CHECK_EQ(leds[0].topRight, -2);
  CHECK_EQ(leds[0].bottomLeft, 3);
  CHECK_EQ(leds[0].bottomRight, 16);
  // The target of opcode 18 is the address field.
  s.send(R"JR(consolefeatures ver=2 type=18 params="A\82FE0000\A\5\1\305419896\1\1\1\255\1\0\1\1431655765\")JR"
         "\r\n");
  CHECK(s.quiet());
  const auto tasks = mock.memoryTasks();
  REQUIRE_EQ(tasks.size(), size_t{1});
  CHECK_EQ(tasks[0].address, 0x82FE0000u);
  CHECK_EQ(tasks[0].value, 0x12345678u);
  CHECK_EQ(tasks[0].useIf, 1u);
  CHECK_EQ(tasks[0].ifValue, 0xFFu);
  CHECK_EQ(tasks[0].useTitle, 0u);
  CHECK_EQ(tasks[0].titleId, 0x55555555u);
  CHECK_EQ(mock.events().back(), std::string("constmem 82FE0000 12345678 00000001 000000FF 00000000 55555555"));
  CHECK_EQ(mock.events().front(), std::string("leds 1 -2 3 16"));
  // Wrong argument counts have no effect.
  s.send(opcode(14, "3", R"(1\1\1\2\1\3\)"));
  CHECK_EQ(s.line(), std::string(kBadParams));
  s.send(opcode(18, "4", R"(1\1\1\2\1\3\1\4\)"));
  CHECK_EQ(s.line(), std::string(kBadParams));
  CHECK_EQ(mock.ledWrites().size(), size_t{1});
  CHECK_EQ(mock.memoryTasks().size(), size_t{1});
}

TEST(JrpcMock, Opcode19HasNoReplyAndNoEffect) {
  JrpcMockServer mock;
  Session s(mock);
  s.send(opcode(19, "1", R"(7/2\0102\)"));
  CHECK(s.quiet());
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("17559"));
  CHECK_EQ(mock.commands()[0].name, std::string("opcode19"));
  CHECK(mock.events().empty());
}

TEST(JrpcMock, ShutdownTakesTheWholeConsoleDownOrOnlyItsConnection) {
  {
    JrpcMockServer mock;
    Session a(mock), b(mock);
    a.send(opcode(11));
    CHECK(a.closed());
    CHECK(b.closed());
    CHECK_EQ(mock.events(), (std::vector<std::string>{"shutdown"}));
    REQUIRE(mock.waitForActiveConnections(0));
  }
  {
    JrpcMockOptions options;
    options.shutdownDropsAllConnections = false;
    options.silentOpcodes = ut::JrpcSilentOpcodeAnswer::SOk;
    JrpcMockServer mock(options);
    Session a(mock), b(mock);
    a.send(opcode(11));
    CHECK_EQ(a.line(), std::string("S_OK"));
    CHECK(a.closed());
    b.send(opcode(13));
    CHECK_EQ(b.line(), std::string("17559"));
  }
}

// --- The receive buffer (section 1.3) ---

namespace {
// A line of exactly `total` bytes counting its LF, built from a valid command padded
// with spaces before the closing quote's end (the parser ignores white space there).
std::string lineOfTotalSize(size_t total) {
  std::string head = "consolefeatures ver=2 type=13 params=\"A\\0\\A\\0\\\"";
  std::string line = head + std::string(total - head.size() - 1, ' ') + "\n";
  return line;
}
} // namespace

TEST(JrpcMock, TheReceiveBufferHoldsExactly8500BytesIncludingTheLf) {
  JrpcMockServer mock;
  Session s(mock);
  s.send(lineOfTotalSize(8500));
  CHECK_EQ(s.line(), std::string("17559"));
  s.send(lineOfTotalSize(8501));
  CHECK(s.quiet());
  s.send(opcode(16));
  CHECK_EQ(s.line(), std::string("FFFE07D1"));
  const auto records = mock.commands();
  REQUIRE_EQ(records.size(), size_t{3});
  CHECK(!records[0].overLong);
  CHECK_EQ(records[0].length, size_t{8500});
  CHECK(records[1].overLong);
  CHECK_EQ(records[1].length, size_t{8501});
  CHECK_EQ(records[1].name, std::string("overlong"));
  CHECK_EQ(records[1].line.size(), size_t{256});
  CHECK(!records[2].overLong);
}

TEST(JrpcMock, AnOverLongLineIsDroppedWhateverTheSegmentation) {
  JrpcMockServer mock;
  Session s(mock);
  // 40000 bytes in one write, then the same in 1000-byte pieces, then a clean command.
  const std::string huge = "consolefeatures " + std::string(40000, 'a') + "\r\n";
  s.send(huge);
  for (size_t at = 0; at < huge.size(); at += 1000) s.send(std::string_view(huge).substr(at, 1000));
  CHECK(s.quiet());
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("17559"));
  const auto records = mock.commands();
  REQUIRE_EQ(records.size(), size_t{3});
  CHECK_EQ(records[0].length, huge.size());
  CHECK_EQ(records[1].length, huge.size());
  CHECK(records[0].overLong && records[1].overLong);
  CHECK_EQ(records[2].name, std::string("kernel"));
}

TEST(JrpcMock, OverLongPolicies) {
  JrpcMockOptions options;
  options.onOverLong = ut::JrpcOnFailure::ErrorLine;
  options.bufferBytes = 200;
  JrpcMockServer mock(options);
  {
    Session s(mock);
    s.send(std::string(300, 'x') + "\r\n");
    CHECK_EQ(s.line(), std::string("error=line too long"));
    s.send(opcode(13));
    CHECK_EQ(s.line(), std::string("17559"));
  }
  options.onOverLong = ut::JrpcOnFailure::Close;
  mock.setOptions(options);
  {
    Session s(mock);
    s.send(std::string(300, 'x') + "\r\n");
    CHECK(s.closed());
  }
}

// --- Connections ---

TEST(JrpcMock, TheNinthConnectionWaitsForASlot) {
  JrpcMockServer mock;
  std::vector<std::unique_ptr<Session>> held;
  for (int i = 0; i < 8; ++i) {
    held.push_back(std::make_unique<Session>(mock));
    CHECK_EQ(held.back()->banner, std::string("JRPC2 connected"));
  }
  CHECK_EQ(mock.activeConnections(), size_t{8});
  Raw ninth(mock);
  Raw tenth(mock);
  REQUIRE(mock.waitForWaitingConnections(2));
  // Held, not refused: no banner, no close.
  CHECK(ninth.quiet());
  CHECK(tenth.quiet());
  CHECK_EQ(mock.connectionsAccepted(), size_t{8});
  // A slot frees by Bye; the connections are served in arrival order.
  held[3]->send("Bye\r\n");
  CHECK_EQ(ninth.line(), std::string("JRPC2 connected"));
  CHECK(tenth.quiet());
  CHECK_EQ(mock.waitingConnections(), size_t{1});
  // A slot frees by a plain close, too.
  held[5]->close();
  CHECK_EQ(tenth.line(), std::string("JRPC2 connected"));
  tenth.send(opcode(13));
  CHECK_EQ(tenth.line(), std::string("17559"));
  CHECK_EQ(mock.connectionsAccepted(), size_t{10});
}

TEST(JrpcMock, TheConnectionLimitIsAnOptionAndRaisingItFreesHeldConnections) {
  JrpcMockOptions options;
  options.connectionLimit = 1;
  JrpcMockServer mock(options);
  Session a(mock);
  Raw b(mock);
  REQUIRE(mock.waitForWaitingConnections(1));
  CHECK(b.quiet());
  options.connectionLimit = 2;
  mock.setOptions(options);
  CHECK_EQ(b.line(), std::string("JRPC2 connected"));
  CHECK_EQ(mock.waitingConnections(), size_t{0});
}

TEST(JrpcMock, StopReleasesHeldConnectionsAndBlockedCalls) {
  auto mock = std::make_unique<JrpcMockServer>(JrpcMockOptions{.connectionLimit = 1});
  std::promise<void> never;
  auto gate = never.get_future().share();
  mock->registerFunction(0x1000u, [gate](const JrpcCall &) {
    gate.wait_for(30s);
    return JrpcReturn::integer(0);
  });
  Session a(*mock);
  Raw b(*mock);
  REQUIRE(mock->waitForWaitingConnections(1));
  a.send(byAddress(1, "1000", "0"));
  REQUIRE(mock->waitForCommands(1));
  never.set_value();
  const auto start = std::chrono::steady_clock::now();
  mock->stop();
  mock.reset();
  CHECK(std::chrono::steady_clock::now() - start < 5s);
  CHECK(b.closed());
}

TEST(JrpcMock, ALongCallDoesNotBlockOtherConnectionsAndFunctionsMayCallBack) {
  JrpcMockServer mock;
  std::promise<void> release;
  auto gate = release.get_future().share();
  std::atomic<size_t> seenCommands{0};
  mock.registerFunction(0x1000u, [&, gate](const JrpcCall &) {
    // No mock lock is held while the function runs.
    seenCommands = mock.commandLines().size();
    gate.wait();
    return JrpcReturn::integer(0x77);
  });
  Session a(mock), b(mock);
  a.send(byAddress(1, "1000", "0"));
  REQUIRE(mock.waitForCommands(1));
  b.send(opcode(13));
  CHECK_EQ(b.line(), std::string("17559"));
  CHECK(a.quiet());
  release.set_value();
  CHECK_EQ(a.line(), std::string("77"));
  CHECK_EQ(seenCommands.load(), size_t{1});
}

TEST(JrpcMock, DropAllConnectionsClosesEveryone) {
  JrpcMockServer mock;
  Session a(mock), b(mock);
  mock.dropAllConnections();
  CHECK(a.closed());
  CHECK(b.closed());
  REQUIRE(mock.waitForActiveConnections(0));
  Session c(mock);
  CHECK_EQ(c.banner, std::string("JRPC2 connected"));
}

TEST(JrpcMock, ConnectionsAreNumberedInArrivalOrder) {
  JrpcMockServer mock;
  Session a(mock), b(mock);
  b.send(opcode(13));
  CHECK_EQ(b.line(), std::string("17559"));
  a.send(opcode(16));
  CHECK_EQ(a.line(), std::string("FFFE07D1"));
  const auto records = mock.commands();
  REQUIRE_EQ(records.size(), size_t{2});
  CHECK_EQ(records[0].connection, size_t{1});
  CHECK_EQ(records[1].connection, size_t{0});
}

// --- Faults ---

TEST(JrpcMock, FaultsAreTargetedAndCounted) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, [](const JrpcCall &) { return JrpcReturn::integer(0x12); });
  mock.registerFunction(0x2000u, [](const JrpcCall &) { return JrpcReturn::integer(0x34); });
  Session s(mock);
  mock.inject(JrpcFault::replyLine("odd").onType(1).onAddress(0x2000).after(1));
  s.send(byAddress(1, "1000", "0"));
  CHECK_EQ(s.line(), std::string("12"));
  s.send(byAddress(1, "2000", "0"));
  CHECK_EQ(s.line(), std::string("34"));   // skipped once
  s.send(byAddress(8, "2000", "0"));
  CHECK_EQ(s.line(), std::string("34"));   // other type
  s.send(byAddress(1, "2000", "0"));
  CHECK_EQ(s.line(), std::string("odd"));
  CHECK_EQ(mock.pendingFaults(), size_t{0});
  s.send(byAddress(1, "2000", "0"));
  CHECK_EQ(s.line(), std::string("34"));

  mock.inject(JrpcFault::replyLine("by text").whenContains("1000").repeat(2));
  s.send(byAddress(1, "1000", "0"));
  CHECK_EQ(s.line(), std::string("by text"));
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("17559"));
  s.send(byAddress(1, "1000", "0"));
  CHECK_EQ(s.line(), std::string("by text"));
  s.send(byAddress(1, "1000", "0"));
  CHECK_EQ(s.line(), std::string("12"));

  // A fault by connection leaves the others alone; always() never runs out.
  mock.inject(JrpcFault::replyLine("only b").onConnection(1).always());
  Session t(mock);
  t.send(opcode(13));
  CHECK_EQ(t.line(), std::string("only b"));
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("17559"));
  t.send(opcode(13));
  CHECK_EQ(t.line(), std::string("only b"));
  mock.clearFaults();
  t.send(opcode(13));
  CHECK_EQ(t.line(), std::string("17559"));
}

TEST(JrpcMock, ErrorAndDebugLines) {
  JrpcMockServer mock;
  Session s(mock);
  mock.inject(JrpcFault::errorLine("Version mismatch").onType(13));
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("error=Version mismatch"));
  mock.inject(JrpcFault::debugLine().onType(16));
  s.send(opcode(16));
  CHECK(s.line().find("DEBUG") != std::string::npos);
  // The reply was replaced, the command was not: the faults only change the answer.
  mock.registerFunction(0x1000u, [](const JrpcCall &) { return JrpcReturn::integer(1); });
  mock.inject(JrpcFault::errorLine("nope").onType(1));
  s.send(byAddress(1, "1000", "0"));
  CHECK_EQ(s.line(), std::string("error=nope"));
  CHECK_EQ(mock.callCount(0x1000), size_t{1});
  mock.inject(JrpcFault::errorLine("nope").onType(1).withoutCommand());
  s.send(byAddress(1, "1000", "0"));
  CHECK_EQ(s.line(), std::string("error=nope"));
  CHECK_EQ(mock.callCount(0x1000), size_t{1});
  // Faults reach opcodes without a reply, and withoutCommand stops their effect.
  mock.inject(JrpcFault::errorLine("busy").onType(14).withoutCommand());
  s.send(opcode(14, "4", R"(1\1\1\2\1\3\1\4\)"));
  CHECK_EQ(s.line(), std::string("error=busy"));
  CHECK(mock.ledWrites().empty());
}

TEST(JrpcMock, DropsMidReplyAndAtOnce) {
  JrpcMockServer mock;
  {
    Session s(mock);
    mock.inject(JrpcFault::dropAfterBytes(2).onType(13));
    s.send(opcode(13));
    CHECK_EQ(s.drain(), std::string("17"));
    CHECK(s.closed());
  }
  {
    Session s(mock);
    mock.inject(JrpcFault::dropConnection().onType(13));
    s.send(opcode(13));
    CHECK(s.closed());
  }
  {
    // A drop on an opcode that has no reply of its own.
    Session s(mock);
    mock.inject(JrpcFault::dropConnection().onType(14));
    s.send(opcode(14, "4", R"(1\1\1\2\1\3\1\4\)"));
    CHECK(s.closed());
  }
  REQUIRE(mock.waitForActiveConnections(0));
}

TEST(JrpcMock, SilenceAndDelay) {
  JrpcMockServer mock;
  Session s(mock);
  mock.inject(JrpcFault::delay(250ms).onType(13));
  const auto start = std::chrono::steady_clock::now();
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("17559"));
  CHECK(std::chrono::steady_clock::now() - start >= 200ms);

  mock.inject(JrpcFault::silence().onType(16));
  s.send(opcode(16));
  CHECK(s.quiet(300ms));
  // Never answers, but is still listening: closing ends it at once.
  s.close();
  REQUIRE(mock.waitForActiveConnections(0));
}

TEST(JrpcMock, StallAfterSomeBytesThenCarryOn) {
  JrpcMockServer mock;
  Session s(mock);
  mock.inject(JrpcFault::stall(3, 200ms).onType(13));
  s.send(opcode(13));
  CHECK_EQ(s.bytes(3), std::string("175"));
  CHECK(s.quiet(80ms));
  CHECK_EQ(s.line(), std::string("59"));
}

TEST(JrpcMock, WrongShapeOversizedEndlessAndTrickledReplies) {
  JrpcMockServer mock;
  {
    Session s(mock);
    mock.inject(JrpcFault::reply("no terminator").onType(13));
    s.send(opcode(13));
    CHECK_EQ(s.drain(), std::string("no terminator"));
  }
  {
    Session s(mock);
    mock.inject(JrpcFault::oversizedLine().onType(13));
    s.send(opcode(13));
    const std::string all = s.drain(300ms);
    CHECK_EQ(all.size(), size_t{64 * 1024 + 1 + 2});
    CHECK_EQ(all.substr(all.size() - 2), std::string("\r\n"));
  }
  {
    Session s(mock);
    mock.inject(JrpcFault::endlessLine().onType(13));
    s.send(opcode(13));
    const std::string head = s.bytes(20000);
    CHECK_EQ(head.size(), size_t{20000});
    CHECK_EQ(head.substr(0, 4), std::string("12xx"));
    CHECK(head.find('\n') == std::string::npos);
    s.close();
    REQUIRE(mock.waitForActiveConnections(0));
  }
  {
    Session s(mock);
    mock.inject(JrpcFault::trickleBytes().onType(13));
    s.send(opcode(13));
    CHECK_EQ(s.line(), std::string("17559"));
  }
  {
    Session s(mock);
    mock.inject(JrpcFault::extraBytes(ut::bytesOf("12\r\n")).onType(13));
    s.send(opcode(13));
    CHECK_EQ(s.line(), std::string("17559"));
    CHECK_EQ(s.line(), std::string("12"));
  }
}

TEST(JrpcMock, HostileRepliesAreDeterministicAndDamaged) {
  JrpcMockServer mock;
  auto run = [&](uint64_t seed) {
    Session s(mock);
    mock.inject(JrpcFault::hostile(seed).onType(13));
    s.send(opcode(13));
    return s.drain();
  };
  bool damaged = false;
  for (uint64_t seed = 1; seed <= 20; ++seed) {
    const std::string first = run(seed);
    CHECK_EQ(run(seed), first);
    if (first != "17559\r\n") damaged = true;
  }
  CHECK(damaged);
}

// --- Records and options ---

TEST(JrpcMock, RecordsAndClearing) {
  JrpcMockServer mock;
  mock.registerFunction(0x1000u, [](const JrpcCall &) { return JrpcReturn::integer(0); });
  Session s(mock);
  s.send(byAddress(0, "1000", "0"));
  CHECK_EQ(s.line(), std::string("0"));
  s.send(opcode(13));
  CHECK_EQ(s.line(), std::string("17559"));
  CHECK_EQ(mock.commandLines(), (std::vector<std::string>{R"(consolefeatures ver=2 type=0 as=0 params="A\1000\A\0\")",
                                                          R"(consolefeatures ver=2 type=13 params="A\0\A\0\")"}));
  CHECK_EQ(mock.commands()[0].name, std::string("call"));
  mock.clearCommands();
  CHECK(mock.commands().empty());
  CHECK(mock.calls().empty());
}

TEST(JrpcMock, OptionsChangeTheNextLine) {
  JrpcMockServer mock;
  Session s(mock);
  s.send(opcode(10));
  CHECK_EQ(s.line(), std::string("A1B2C3D4E5F60718"));
  auto options = mock.options();
  options.cpuKeyDigits = 16;
  mock.setOptions(options);
  s.send(opcode(10));
  CHECK_EQ(s.line(), std::string("00000000A1B2C3D4" "00000000E5F60718"));
}

// --- Real sockets ---

TEST(JrpcMock, ServesOverLoopbackTcp) {
  JrpcMockServer mock;
  mock.registerFunction(0x82010000u, [](const JrpcCall &call) {
    return JrpcReturn::integer(static_cast<uint64_t>(call.args.at(0).integer + call.args.at(1).integer));
  });
  auto port = mock.listenTcp();
  if (!port) SKIP("loopback TCP refused: " + updclient::formatError(port.error()));
  auto transport = updclient::net::TcpTransport::connect("127.0.0.1", *port, 5000ms);
  REQUIRE_OK(transport);
  Raw raw(std::move(*transport));
  CHECK_EQ(raw.line(), std::string("JRPC2 connected"));
  raw.send(R"JR(consolefeatures ver=2 type=1 as=0 params="A\82010000\A\2\1\5\1\16\")JR"
           "\r\n");
  CHECK_EQ(raw.line(), std::string("15"));
  raw.send(opcode(13));
  CHECK_EQ(raw.line(), std::string("17559"));
  raw.send("Bye\r\n");
  CHECK(raw.closed());
  CHECK_EQ(mock.tcpPort(), *port);
}

TEST(JrpcMock, TcpConnectionBeyondTheLimitWaitsToo) {
  JrpcMockOptions options;
  options.connectionLimit = 1;
  JrpcMockServer mock(options);
  auto port = mock.listenTcp();
  if (!port) SKIP("loopback TCP refused: " + updclient::formatError(port.error()));
  auto first = updclient::net::TcpTransport::connect("127.0.0.1", *port, 5000ms);
  auto second = updclient::net::TcpTransport::connect("127.0.0.1", *port, 5000ms);
  REQUIRE_OK(first);
  REQUIRE_OK(second);
  Raw a(std::move(*first));
  Raw b(std::move(*second));
  CHECK_EQ(a.line(), std::string("JRPC2 connected"));
  CHECK(b.quiet());
  a.send("Bye\r\n");
  CHECK_EQ(b.line(), std::string("JRPC2 connected"));
}
