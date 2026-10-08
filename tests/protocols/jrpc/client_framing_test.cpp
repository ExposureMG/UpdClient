#include "protocols/jrpc/client_fake.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/jrpc/client.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

// How the client reads a banner and a reply off the wire: line ends, split reads, the
// size limits, and what it does with an answer of the wrong shape (D3, D4). The console
// is the scripted fake of client_fake.hpp.

using namespace updclient;
using namespace updclient::jrpc;
using jt::FakeConsole;

namespace {

struct Shape {
  std::string name;
  CallSpec spec;
  std::string reply; // without the terminator
  CallValue value;
};

// One well-formed reply for every return kind.
std::vector<Shape> shapes() {
  return {
      {"void", jt::callAt(0x82000000), "S_OK", CallValue(std::monostate{})},
      {"void hex", jt::callAt(0x82000000), "0", CallValue(std::monostate{})},
      {"int", jt::callAt(0x82000000, ReturnKind::Int), "2A", CallValue(uint64_t{42})},
      {"byte", jt::callAt(0x82000000, ReturnKind::Byte), "FF", CallValue(uint64_t{255})},
      {"int64", jt::callAt(0x82000000, ReturnKind::Int64), "248173A00", CallValue(uint64_t{0x248173A00})},
      {"float", jt::callAt(0x82000000, ReturnKind::Float), "1.500000", CallValue(1.5)},
      {"string", jt::callAt(0x82000000, ReturnKind::String), "hello world", CallValue(std::string("hello world"))},
      {"ints", jt::callAt(0x82000000, ReturnKind::IntArray, {}, 3), "1,-2,3;",
       CallValue(std::vector<int32_t>{1, -2, 3})},
      {"floats", jt::callAt(0x82000000, ReturnKind::FloatArray, {}, 2), "1.500000,2.000000;",
       CallValue(std::vector<double>{1.5, 2.0})},
      {"bytes", jt::callAt(0x82000000, ReturnKind::ByteArray, {}, 3), "1,FF,A;",
       CallValue(std::vector<uint8_t>{1, 255, 10})},
  };
}

} // namespace

// --- The banner --------------------------------------------------------------------

TEST(JrpcClientBanner, ExactBannerIsAccepted) {
  auto console = FakeConsole::create();
  auto client = jt::attach(console);
  REQUIRE_OK(client);
  CHECK(client->isConnected());
  CHECK(!client->lastDelivery().has_value());
  CHECK_EQ(console->written(), size_t{0});
  CHECK_EQ(console->lastTimeout(), std::chrono::milliseconds(200));
}

TEST(JrpcClientBanner, SurvivesEveryReadSplit) {
  const size_t length = jt::kBannerLine.size();
  for (size_t k = 1; k < length; ++k) {
    auto console = FakeConsole::create();
    console->planReads({k});
    auto client = jt::attach(console);
    CHECK_MSG(client.has_value(), "split at " + std::to_string(k));
    if (!client) continue;
    console->on(std::string(jt::kIntCommand), "2A\r\n");
    CHECK_MSG(client->call(jt::intCall()).has_value(), "call after split at " + std::to_string(k));
  }
  auto console = FakeConsole::create();
  console->setMaxRead(1);
  CHECK_OK(jt::attach(console));
}

TEST(JrpcClientBanner, BareLineFeedIsTolerated) {
  auto console = FakeConsole::create(false);
  console->send(std::string_view("JRPC2 connected\n"));
  CHECK_OK(jt::attach(console));
}

TEST(JrpcClientBanner, AnythingElseIsAProtocolError) {
  for (const char *text : {"JRPC2 connected ", "jrpc2 connected", "JRPC2 connected!", "JRPC connected", "", " JRPC2 connected",
                           "201- connected", "200- OK", "Bye"}) {
    auto console = FakeConsole::create(false);
    console->line(text);
    auto client = jt::attach(console);
    CHECK_MSG(!client.has_value() && client.error().code == ErrorCode::Protocol, std::string("banner '") + text + "'");
    CHECK_MSG(console->closed(), std::string("closed after '") + text + "'");
    if (!client) CHECK(client.error().message.find("JRPC2 connected") != std::string::npos);
  }
}

TEST(JrpcClientBanner, ADebugLineMeansJrpcIsNotInstalled) {
  auto console = FakeConsole::create(false);
  console->line("201- DEBUG connected");
  auto client = jt::attach(console);
  REQUIRE_ERR(client, ErrorCode::Unsupported);
  CHECK(client.error().message.find("not installed") != std::string::npos);
  CHECK(console->closed());
  CHECK_EQ(console->byes(), 0);
}

TEST(JrpcClientBanner, SilenceIsATimeoutAndNoBye) {
  auto console = FakeConsole::create(false);
  auto client = jt::attach(console);
  REQUIRE_ERR(client, ErrorCode::Timeout);
  CHECK(console->closed());
  CHECK_EQ(console->written(), size_t{0});
  // A 9th connection waits for a slot and gets no banner: the message says so.
  CHECK(client.error().message.find("8 connections") != std::string::npos);

  auto half = FakeConsole::create(false);
  half->send(std::string_view("JRPC2 conn"));
  CHECK_ERR(jt::attach(half), ErrorCode::Timeout);
}

TEST(JrpcClientBanner, ClosedBeforeTheBannerIsDisconnected) {
  auto console = FakeConsole::create(false);
  console->hangUp();
  CHECK_ERR(jt::attach(console), ErrorCode::Disconnected);

  auto cut = FakeConsole::create(false);
  cut->send(std::string_view("JRPC2 conn")).hangUp();
  auto client = jt::attach(cut);
  REQUIRE_ERR(client, ErrorCode::Disconnected);
  CHECK(client.error().message.find("middle of a line") != std::string::npos);
}

TEST(JrpcClientBanner, ALongBannerIsRefused) {
  auto console = FakeConsole::create(false);
  console->line(std::string(600, 'J'));
  CHECK_ERR(jt::attach(console), ErrorCode::LimitExceeded);

  // No line end at all: refused once the limit is passed, not at the end.
  auto endless = FakeConsole::create(false);
  endless->send(std::string(100000, 'J'));
  CHECK_ERR(jt::attach(endless), ErrorCode::LimitExceeded);
  CHECK(endless->unread() > 0);

  // maxReplyBytes lowers the limit further.
  ClientOptions options = jt::quickOptions();
  options.maxReplyBytes = 10;
  auto small = FakeConsole::create();
  CHECK_ERR(jt::attach(small, options), ErrorCode::LimitExceeded);
}

TEST(JrpcClientBanner, BytesSentWithTheBannerAreCaughtAtTheFirstCall) {
  // The banner and a reply in one segment: a reply before any command belongs to no
  // command, and taking it for the first call's answer could hand a caller garbage.
  auto console = FakeConsole::create();
  console->send(std::string_view("2A\r\n"));
  auto client = jt::connected(console);
  CHECK(client.isConnected());
  auto r = client.call(jt::intCall());
  REQUIRE_ERR(r, ErrorCode::Protocol);
  CHECK(r.error().message.find("4 bytes") != std::string::npos);
  CHECK(!client.isConnected());
  CHECK(console->commands().empty());
  CHECK_EQ(console->written(), size_t{0});
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK(delivery->delivery == Delivery::NotSent);
}

// --- Reply framing -----------------------------------------------------------------

TEST(JrpcClientFraming, EveryReplyShapeDecodes) {
  for (const Shape &shape : shapes()) {
    auto console = FakeConsole::create();
    console->handle([&](FakeConsole &c, const std::string &) { c.line(shape.reply); });
    auto client = jt::connected(console);
    auto r = client.call(shape.spec);
    CHECK_MSG(r.has_value(), shape.name);
    if (!r) continue;
    CHECK_MSG(r->value == shape.value, shape.name);
    CHECK_EQ(r->line, shape.reply);
    CHECK(client.isConnected());
  }
}

TEST(JrpcClientFraming, EveryReplySurvivesEveryReadSplit) {
  for (const Shape &shape : shapes()) {
    const size_t length = shape.reply.size() + 2;
    for (size_t k = 1; k < length; ++k) {
      auto console = FakeConsole::create();
      console->handle([&](FakeConsole &c, const std::string &) { c.line(shape.reply); });
      auto client = jt::connected(console);
      console->planReads({k});
      auto r = client.call(shape.spec);
      const std::string note = shape.name + " split at " + std::to_string(k);
      CHECK_MSG(r.has_value(), note);
      if (r) CHECK_MSG(r->value == shape.value, note);
      CHECK_MSG(client.isConnected(), note);
    }
    // Byte by byte, and two replies in a row on one connection.
    auto console = FakeConsole::create();
    console->setMaxRead(1);
    console->handle([&](FakeConsole &c, const std::string &) { c.line(shape.reply); });
    auto client = jt::connected(console);
    for (int round = 0; round < 2; ++round) {
      auto r = client.call(shape.spec);
      CHECK_MSG(r.has_value(), shape.name + " byte by byte");
      if (r) CHECK_MSG(r->value == shape.value, shape.name);
    }
  }
}

TEST(JrpcClientFraming, TheCommandGoesOutWithCrLf) {
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "2A\r\n");
  auto client = jt::connected(console);
  REQUIRE_OK(client.call(jt::intCall()));
  CHECK_EQ(console->wire(), std::string(jt::kIntCommand) + "\r\n");
  CHECK_EQ(console->problems(), std::string());
}

TEST(JrpcClientFraming, BareLineFeedsAreTolerated) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &) { c.send(std::string_view("2A\n")); });
  auto client = jt::connected(console);
  auto r = client.call(jt::intCall());
  REQUIRE_OK(r);
  CHECK(r->value == CallValue(uint64_t{42}));
  CHECK_OK(client.call(jt::intCall()));
}

TEST(JrpcClientFraming, ACarriageReturnAloneIsNotAnEnd) {
  // CR is not a terminator: the reply never completes and the call times out.
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &) { c.send(std::string_view("2A\r")); });
  auto client = jt::connected(console);
  auto r = client.call(jt::intCall());
  REQUIRE_ERR(r, ErrorCode::Timeout);
  CHECK(!client.isConnected());
  CHECK(console->closed());
}

TEST(JrpcClientFraming, ExactlyOneCarriageReturnIsStripped) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &) { c.send(std::string_view("2A\r\r\n")); });
  auto client = jt::connected(console);
  // "2A\r" is not hex, and the client does not strip a second CR to make it one.
  CHECK_ERR(client.call(jt::intCall()), ErrorCode::Protocol);
  CHECK(!client.isConnected());

  auto text = FakeConsole::create();
  text->handle([](FakeConsole &c, const std::string &) { c.send(std::string_view("a\rb\r\n")); });
  auto second = jt::connected(text);
  // A string result may not hold a CR (the framing would be wrong).
  CHECK_ERR(second.call(jt::callAt(0x82000000, ReturnKind::String)), ErrorCode::Protocol);
}

TEST(JrpcClientFraming, AnEmptyReplyIsAProtocolErrorForEveryKind) {
  for (const Shape &shape : shapes()) {
    auto console = FakeConsole::create();
    console->handle([](FakeConsole &c, const std::string &) { c.line(""); });
    auto client = jt::connected(console);
    auto r = client.call(shape.spec);
    // An empty string is a legal string result; every other kind needs a value.
    if (shape.name == "string") {
      CHECK_OK(r);
    } else {
      CHECK_MSG(!r.has_value() && r.error().code == ErrorCode::Protocol, shape.name);
      CHECK_MSG(!client.isConnected(), shape.name);
    }
  }
}

TEST(JrpcClientFraming, MaxReplyBytesCountsTheLineWithoutItsTerminator) {
  ClientOptions options = jt::quickOptions();
  options.maxReplyBytes = 64;
  const auto spec = jt::callAt(0x82000000, ReturnKind::String);
  for (const char *terminator : {"\r\n", "\n"}) {
    {
      auto console = FakeConsole::create();
      console->handle([&](FakeConsole &c, const std::string &) { c.send(std::string(64, 'x') + terminator); });
      auto client = jt::connected(console, options);
      auto r = client.call(spec);
      REQUIRE_OK(r);
      CHECK_EQ(r->line.size(), size_t{64});
      CHECK(client.isConnected());
    }
    {
      auto console = FakeConsole::create();
      console->handle([&](FakeConsole &c, const std::string &) { c.send(std::string(65, 'x') + terminator); });
      auto client = jt::connected(console, options);
      CHECK_ERR(client.call(spec), ErrorCode::LimitExceeded);
      CHECK(!client.isConnected());
      CHECK(console->closed());
    }
  }
  // A line that never ends is refused once the limit is passed, not at the end.
  auto endless = FakeConsole::create();
  endless->handle([](FakeConsole &c, const std::string &) { c.send(std::string(100000, 'y')); });
  auto client = jt::connected(endless, options);
  CHECK_ERR(client.call(spec), ErrorCode::LimitExceeded);
  CHECK(endless->unread() > 0);
  CHECK(!client.isConnected());

  // The default is 64 KiB.
  CHECK_EQ(ClientOptions().maxReplyBytes, size_t{64 * 1024});
  auto big = FakeConsole::create();
  big->handle([](FakeConsole &c, const std::string &) { c.line(std::string(64 * 1024, 'z')); });
  auto bigClient = jt::connected(big);
  CHECK_OK(bigClient.call(spec));
  auto tooBig = FakeConsole::create();
  tooBig->handle([](FakeConsole &c, const std::string &) { c.line(std::string(64 * 1024 + 1, 'z')); });
  auto tooBigClient = jt::connected(tooBig);
  CHECK_ERR(tooBigClient.call(spec), ErrorCode::LimitExceeded);
}

TEST(JrpcClientFraming, ALongLineArrivingInPiecesIsReadInLinearTime) {
  auto console = FakeConsole::create();
  console->setMaxRead(7);
  console->handle([](FakeConsole &c, const std::string &) { c.line(std::string(60000, 'q')); });
  auto client = jt::connected(console);
  const auto start = std::chrono::steady_clock::now();
  auto r = client.call(jt::callAt(0x82000000, ReturnKind::String));
  REQUIRE_OK(r);
  CHECK_EQ(r->line.size(), size_t{60000});
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
}

TEST(JrpcClientFraming, ExtraBytesAfterAReplyAreCaughtAtTheNextCall) {
  {
    // Two lines for one command.
    auto console = FakeConsole::create();
    console->on(std::string(jt::kIntCommand), "2A\r\n2B\r\n");
    auto client = jt::connected(console);
    auto first = client.call(jt::intCall());
    REQUIRE_OK(first);
    CHECK(first->value == CallValue(uint64_t{42}));
    auto second = client.call(jt::intCall());
    REQUIRE_ERR(second, ErrorCode::Protocol);
    CHECK(second.error().message.find("4 bytes") != std::string::npos);
    CHECK(!client.isConnected());
    CHECK_EQ(console->commands().size(), size_t{1});
  }
  {
    // Bytes without a line end, left over after a good reply.
    auto console = FakeConsole::create();
    console->on(std::string(jt::kIntCommand), "2A\r\nxyz");
    auto client = jt::connected(console);
    CHECK_OK(client.call(jt::intCall()));
    CHECK_ERR(client.call(jt::intCall()), ErrorCode::Protocol);
  }
}

TEST(JrpcClientFraming, TheLeftoverDoesNotStopTheByeFromBeingWritten) {
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "2A\r\nxyz");
  auto client = jt::connected(console);
  CHECK_OK(client.call(jt::intCall()));
  client.close();
  CHECK_EQ(console->byes(), 1);
  CHECK(console->closed());
}

// --- Failures of the answer --------------------------------------------------------

TEST(JrpcClientFailure, AnErrorLineKeepsTheConnection) {
  auto console = FakeConsole::create();
  console->on(std::string(jt::kIntCommand), "error=Version mismatch\r\n").on(std::string(jt::kIntCommand), "2A\r\n");
  auto client = jt::connected(console);
  auto r = client.call(jt::intCall());
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK(client.isConnected());
  CHECK(r.error().message.ends_with("error=Version mismatch"));
  auto delivery = client.lastDelivery();
  REQUIRE(delivery.has_value());
  CHECK(delivery->delivery == Delivery::Answered);
  // The same connection serves the next call.
  auto next = client.call(jt::intCall());
  REQUIRE_OK(next);
  CHECK(next->value == CallValue(uint64_t{42}));
  CHECK_EQ(console->problems(), std::string());
  CHECK(!console->closed());
}

TEST(JrpcClientFailure, ErrorTextsAreClassified) {
  struct Row {
    std::string text;
    RemoteFault fault;
  };
  const std::vector<Row> rows = {
      {"Could not resolve function address, params = A\\82010000\\A\\0\\, 1", RemoteFault::CouldNotResolve},
      {"Could not resolve function address", RemoteFault::CouldNotResolve},
      {"Version mismatch", RemoteFault::VersionMismatch},
      {"The paramaters were not found", RemoteFault::ParametersNotFound},
      {"The parameters were not found", RemoteFault::Other}, // the server's typo is part of the text
      {"version mismatch", RemoteFault::Other},               // case matters
      {"something else", RemoteFault::Other},
      {"", RemoteFault::Other},
  };
  for (const Row &row : rows) {
    auto console = FakeConsole::create();
    console->handle([&](FakeConsole &c, const std::string &) { c.line("error=" + row.text); });
    auto client = jt::connected(console);
    auto r = client.call(jt::intCall());
    REQUIRE_ERR(r, ErrorCode::Io);
    CHECK_MSG(remoteFault(r.error()) == std::optional<RemoteFault>(row.fault), "text '" + row.text + "'");
    CHECK_EQ(r.error().sysError, 1000 + static_cast<int>(row.fault));
    CHECK_MSG(r.error().message.find("console answered error=") != std::string::npos, row.text);
    CHECK_MSG(client.isConnected(), row.text);
  }
}

TEST(JrpcClientFailure, ErrorTextIsKeptPrintableAndShortened) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &) {
    c.line("error=bad\x01\x7f\xff " + std::string(200, 'z'));
  });
  auto client = jt::connected(console);
  auto r = client.call(jt::intCall());
  REQUIRE_ERR(r, ErrorCode::Io);
  for (char c : r.error().message) {
    CHECK(static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F);
  }
  CHECK(r.error().message.find("\\x01\\x7f\\xff") != std::string::npos);
  CHECK(r.error().message.ends_with("..."));
  CHECK(r.error().message.size() < 200);
  CHECK(remoteFault(r.error()) == std::optional<RemoteFault>(RemoteFault::Other));
}

TEST(JrpcClientFailure, OnlyAnErrorLineIsARemoteFault) {
  CHECK(!remoteFault(makeError(ErrorCode::Io, "disk", 1001)).has_value());
  CHECK(!remoteFault(makeError(ErrorCode::Timeout, "call: console answered error=x", 1000)).has_value());
  CHECK(!remoteFault(makeError(ErrorCode::Io, "call: console answered error=x", 5)).has_value());
  CHECK(!remoteFault(makeError(ErrorCode::Io, "call: console answered error=x", 1004)).has_value());
  CHECK(!remoteFault(makeError(ErrorCode::Protocol, "x")).has_value());
  CHECK(remoteFault(makeError(ErrorCode::Io, "call: console answered error=x", 1000)).has_value());
}

TEST(JrpcClientFailure, ADebugLineMeansJrpcIsNotInstalled) {
  auto console = FakeConsole::create();
  console->handle([](FakeConsole &c, const std::string &) { c.line("DEBUG: unknown command"); });
  auto client = jt::connected(console);
  auto r = client.call(jt::intCall());
  REQUIRE_ERR(r, ErrorCode::Unsupported);
  CHECK(r.error().message.find("not installed") != std::string::npos);
  CHECK(!client.isConnected());
  CHECK(console->closed());

  // An error= line is still an error= line when it mentions the word.
  auto errorLine = FakeConsole::create();
  errorLine->handle([](FakeConsole &c, const std::string &) { c.line("error=DEBUG build"); });
  auto second = jt::connected(errorLine);
  CHECK_ERR(second.call(jt::intCall()), ErrorCode::Io);
  CHECK(second.isConnected());
}

TEST(JrpcClientFailure, AReplyOfTheWrongShapeClosesTheConnection) {
  struct Row {
    ReturnKind kind;
    size_t arraySize;
    std::string reply;
  };
  const std::vector<Row> rows = {
      {ReturnKind::Void, 0, "hello"},
      {ReturnKind::Void, 0, "0x1F"},
      {ReturnKind::Void, 0, "S_OK "},
      {ReturnKind::Int, 0, "xyz"},
      {ReturnKind::Int, 0, "123456789"},
      {ReturnKind::Int, 0, "-1"},
      {ReturnKind::Int, 0, "1.5"},
      {ReturnKind::Int, 0, " 1"},
      {ReturnKind::Byte, 0, "G"},
      {ReturnKind::Int64, 0, "12345678901234567"},
      {ReturnKind::Float, 0, "abc"},
      {ReturnKind::Float, 0, "1.5 "},
      {ReturnKind::IntArray, 3, "1,2;"},
      {ReturnKind::IntArray, 3, "1,2,3"},
      {ReturnKind::IntArray, 3, "1,2,3;x"},
      {ReturnKind::IntArray, 3, "1,2,3,;"},
      {ReturnKind::IntArray, 3, "2A"},
      {ReturnKind::FloatArray, 2, "1.5;"},
      {ReturnKind::ByteArray, 2, "1,2,3;"},
      {ReturnKind::ByteArray, 2, "1,ZZ;"},
  };
  for (const Row &row : rows) {
    auto console = FakeConsole::create();
    console->handle([&](FakeConsole &c, const std::string &) { c.line(row.reply); });
    auto client = jt::connected(console);
    auto r = client.call(jt::callAt(0x82000000, row.kind, {}, row.arraySize));
    const std::string note = "reply '" + row.reply + "'";
    CHECK_MSG(!r.has_value() && r.error().code == ErrorCode::Protocol, note);
    CHECK_MSG(!client.isConnected(), note);
    CHECK_MSG(console->closed(), note);
    CHECK_MSG(console->byes() == 0, note);
    auto delivery = client.lastDelivery();
    CHECK_MSG(delivery && delivery->delivery == Delivery::Answered, note);
    // The connection is gone for good: no further command goes out.
    CHECK_ERR(client.call(jt::intCall()), ErrorCode::NotConnected);
    CHECK_EQ(console->commands().size(), size_t{1});
  }
}

TEST(JrpcClientFailure, AClosedConnectionIsDisconnected) {
  {
    auto console = FakeConsole::create();
    console->on(std::string(jt::kIntCommand), "").hangUp();
    auto client = jt::connected(console);
    auto r = client.call(jt::intCall());
    REQUIRE_ERR(r, ErrorCode::Disconnected);
    CHECK(r.error().message.find("closed the connection") != std::string::npos);
    CHECK(!client.isConnected());
  }
  {
    // Cut in the middle of a line.
    auto console = FakeConsole::create();
    console->on(std::string(jt::kIntCommand), "2");
    console->hangUp();
    auto client = jt::connected(console);
    auto r = client.call(jt::intCall());
    REQUIRE_ERR(r, ErrorCode::Disconnected);
    CHECK(r.error().message.find("middle of a line") != std::string::npos);
  }
  {
    // The write fails.
    auto console = FakeConsole::create();
    console->dropAfterWritten(10);
    auto client = jt::connected(console);
    CHECK_ERR(client.call(jt::intCall()), ErrorCode::Disconnected);
    CHECK(!client.isConnected());
  }
}
