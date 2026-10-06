#include "support/mock_transport.hpp"
#include "support/test_harness.hpp"

#include <updclient/net/transport.hpp>

#include <limits>

using namespace updclient;
using ut::Bytes;
using ut::MockScript;

namespace {

std::span<const uint8_t> view(const Bytes &b) {
  return std::span<const uint8_t>(b);
}

} // namespace

TEST(WriteAll, WritesEverythingInOneCall) {
  auto script = MockScript::create();
  auto t = script->transport();
  const Bytes data = ut::patternBytes(100);
  REQUIRE_OK(t->writeAll(view(data)));
  CHECK_EQ(script->written(), data);
  CHECK_EQ(script->writeCalls(), size_t{1});
}

TEST(WriteAll, LoopsOverPartialWrites) {
  auto script = MockScript::create();
  script->setMaxWrite(7);
  auto t = script->transport();
  const Bytes data = ut::patternBytes(100);
  REQUIRE_OK(t->writeAll(view(data)));
  CHECK_EQ(script->written(), data);
  CHECK_EQ(script->writeCalls(), size_t{15});
}

TEST(WriteAll, SingleByteWrites) {
  auto script = MockScript::create();
  script->setMaxWrite(1);
  auto t = script->transport();
  const Bytes data = ut::patternBytes(33, 4);
  REQUIRE_OK(t->writeAll(view(data)));
  CHECK_EQ(script->written(), data);
}

TEST(WriteAll, EmptyBufferSucceedsWithoutWriting) {
  auto script = MockScript::create();
  auto t = script->transport();
  CHECK_OK(t->writeAll({}));
  CHECK_EQ(script->writeCalls(), size_t{0});
}

TEST(WriteAll, ZeroProgressIsAnIoError) {
  auto script = MockScript::create();
  script->stallWrites();
  auto t = script->transport();
  const Bytes data = ut::bytesOf("abc");
  CHECK_ERR(t->writeAll(view(data)), ErrorCode::Io);
  CHECK_EQ(script->writeCalls(), size_t{1});
}

TEST(WriteAll, ErrorMidwayPropagatesAfterPartialWrite) {
  auto script = MockScript::create();
  script->setMaxWrite(4);
  script->failWritesAfter(10, makeError(ErrorCode::Disconnected, "peer reset", 104));
  auto t = script->transport();
  const Bytes data = ut::patternBytes(40);
  const auto r = t->writeAll(view(data));
  REQUIRE_ERR(r, ErrorCode::Disconnected);
  CHECK_EQ(r.error().sysError, 104);
  CHECK_EQ(script->written().size(), size_t{10});
}

TEST(WriteAll, ClosedTransportIsNotConnected) {
  auto script = MockScript::create();
  auto t = script->transport();
  t->close();
  CHECK(!t->isOpen());
  const Bytes data = ut::bytesOf("x");
  CHECK_ERR(t->writeAll(view(data)), ErrorCode::NotConnected);
  CHECK_EQ(script->writeCalls(), size_t{0});
}

TEST(ReadExact, ReadsAcrossShortReads) {
  auto script = MockScript::create();
  const Bytes payload = ut::patternBytes(50);
  script->replyChunked(payload, 3);
  auto t = script->transport();
  Bytes out(50);
  REQUIRE_OK(t->readExact(out));
  CHECK_EQ(out, payload);
}

TEST(ReadExact, ReadsAcrossSingleByteReads) {
  auto script = MockScript::create();
  const Bytes payload = ut::patternBytes(20, 9);
  script->reply(payload).setMaxRead(1);
  auto t = script->transport();
  Bytes out(20);
  REQUIRE_OK(t->readExact(out));
  CHECK_EQ(out, payload);
  CHECK_EQ(script->readCalls(), size_t{20});
}

TEST(ReadExact, LeavesSurplusBytesUnread) {
  auto script = MockScript::create();
  script->reply("abcdef");
  auto t = script->transport();
  Bytes out(4);
  REQUIRE_OK(t->readExact(out));
  CHECK_EQ(ut::textOf(out), std::string("abcd"));
  CHECK_EQ(script->unreadBytes(), size_t{2});
}

TEST(ReadExact, EofBeforeEnoughBytesIsDisconnected) {
  auto script = MockScript::create();
  script->reply("abc");
  auto t = script->transport();
  Bytes out(10);
  const auto r = t->readExact(out);
  REQUIRE_ERR(r, ErrorCode::Disconnected);
  CHECK(r.error().message.find("3 of 10") != std::string::npos);
}

TEST(ReadExact, ImmediateEofIsDisconnected) {
  auto script = MockScript::create();
  auto t = script->transport();
  Bytes out(1);
  CHECK_ERR(t->readExact(out), ErrorCode::Disconnected);
}

TEST(ReadExact, TimeoutMidwayPropagates) {
  auto script = MockScript::create();
  script->reply("abc").replyTimeout();
  auto t = script->transport();
  Bytes out(10);
  CHECK_ERR(t->readExact(out), ErrorCode::Timeout);
}

TEST(ReadExact, TransportErrorPropagates) {
  auto script = MockScript::create();
  script->reply("ab").replyError(makeError(ErrorCode::Io, "boom", 5));
  auto t = script->transport();
  Bytes out(10);
  const auto r = t->readExact(out);
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK_EQ(r.error().sysError, 5);
}

TEST(ReadExact, EmptyBufferSucceedsWithoutReading) {
  auto script = MockScript::create();
  auto t = script->transport();
  CHECK_OK(t->readExact({}));
  CHECK_EQ(script->readCalls(), size_t{0});
}

TEST(ReadExact, ClosedTransportIsNotConnected) {
  auto script = MockScript::create();
  script->reply("abc");
  auto t = script->transport();
  t->close();
  Bytes out(3);
  CHECK_ERR(t->readExact(out), ErrorCode::NotConnected);
}

TEST(ReadUntilEof, CollectsEverythingUntilEof) {
  auto script = MockScript::create();
  const Bytes payload = ut::patternBytes(50000, 3);
  script->replyChunked(payload, 4096);
  auto t = script->transport();
  auto r = t->readUntilEof(1 << 20);
  REQUIRE_OK(r);
  CHECK_EQ(*r, payload);
}

TEST(ReadUntilEof, EmptyStreamGivesEmptyResult) {
  auto script = MockScript::create();
  auto t = script->transport();
  auto r = t->readUntilEof(10);
  REQUIRE_OK(r);
  CHECK(r->empty());
}

TEST(ReadUntilEof, ExactlyTheLimitIsAccepted) {
  auto script = MockScript::create();
  const Bytes payload = ut::patternBytes(1000);
  script->replyChunked(payload, 128);
  auto t = script->transport();
  auto r = t->readUntilEof(1000);
  REQUIRE_OK(r);
  CHECK_EQ(r->size(), size_t{1000});
}

TEST(ReadUntilEof, OneByteOverTheLimitIsRejected) {
  auto script = MockScript::create();
  script->replyChunked(ut::patternBytes(1001), 128);
  auto t = script->transport();
  CHECK_ERR(t->readUntilEof(1000), ErrorCode::LimitExceeded);
}

TEST(ReadUntilEof, LargeSingleChunkOverTheLimitIsRejected) {
  auto script = MockScript::create();
  script->reply(ut::patternBytes(100000));
  auto t = script->transport();
  CHECK_ERR(t->readUntilEof(10), ErrorCode::LimitExceeded);
}

TEST(ReadUntilEof, ZeroLimit) {
  {
    auto script = MockScript::create();
    auto t = script->transport();
    CHECK_OK(t->readUntilEof(0));
  }
  {
    auto script = MockScript::create();
    script->reply("x");
    auto t = script->transport();
    CHECK_ERR(t->readUntilEof(0), ErrorCode::LimitExceeded);
  }
}

TEST(ReadUntilEof, UnboundedLimitDoesNotOverflow) {
  auto script = MockScript::create();
  const Bytes payload = ut::patternBytes(40000, 5);
  script->replyChunked(payload, 10000);
  auto t = script->transport();
  auto r = t->readUntilEof(std::numeric_limits<size_t>::max());
  REQUIRE_OK(r);
  CHECK_EQ(*r, payload);
}

TEST(ReadUntilEof, ShortReadsAreAssembled) {
  auto script = MockScript::create();
  const Bytes payload = ut::patternBytes(300, 8);
  script->reply(payload).setMaxRead(1);
  auto t = script->transport();
  auto r = t->readUntilEof(1000);
  REQUIRE_OK(r);
  CHECK_EQ(*r, payload);
}

TEST(ReadUntilEof, TimeoutAndErrorsPropagate) {
  {
    auto script = MockScript::create();
    script->reply("abc").replyTimeout();
    auto t = script->transport();
    CHECK_ERR(t->readUntilEof(100), ErrorCode::Timeout);
  }
  {
    auto script = MockScript::create();
    script->reply("abc").replyError(makeError(ErrorCode::Io, "bad", 1));
    auto t = script->transport();
    CHECK_ERR(t->readUntilEof(100), ErrorCode::Io);
  }
}

TEST(ReadUntilEof, ClosedTransportIsNotConnected) {
  auto script = MockScript::create();
  auto t = script->transport();
  t->close();
  CHECK_ERR(t->readUntilEof(10), ErrorCode::NotConnected);
}

TEST(MockTransport, ExpectedWritesAreCheckedByteForByte) {
  auto script = MockScript::create();
  script->expectWrite("hello world");
  auto t = script->transport();
  const Bytes first = ut::bytesOf("hello ");
  REQUIRE_OK(t->writeAll(view(first)));
  CHECK(!script->problems().empty());
  const Bytes second = ut::bytesOf("world");
  REQUIRE_OK(t->writeAll(view(second)));
  CHECK_EQ(script->problems(), std::string());
}

TEST(MockTransport, DeviatingWriteIsRejectedAndReported) {
  auto script = MockScript::create();
  script->expectWrite("abc");
  auto t = script->transport();
  const Bytes wrong = ut::bytesOf("abd");
  CHECK_ERR(t->writeAll(view(wrong)), ErrorCode::Io);
  CHECK(!script->problems().empty());
}

TEST(MockTransport, ExtraWriteBeyondExpectationIsReported) {
  auto script = MockScript::create();
  script->expectWrite("ab");
  auto t = script->transport();
  const Bytes more = ut::bytesOf("abc");
  CHECK_ERR(t->writeAll(view(more)), ErrorCode::Io);
  CHECK(script->problems().find("extra") != std::string::npos);
}

TEST(MockTransport, DrainedScriptCanTimeOutInsteadOfEof) {
  auto script = MockScript::create();
  script->setEofWhenDrained(false);
  auto t = script->transport();
  Bytes out(1);
  const auto r = t->readSome(out);
  CHECK_ERR(r, ErrorCode::Timeout);
}

TEST(MockTransport, RecordsTimeoutAndClose) {
  auto script = MockScript::create();
  auto t = script->transport();
  CHECK(!script->timeoutWasSet());
  REQUIRE_OK(t->setTimeout(std::chrono::milliseconds(1234)));
  CHECK(script->timeoutWasSet());
  CHECK_EQ(script->lastTimeout().count(), 1234);
  t->close();
  CHECK(script->closed());
}
