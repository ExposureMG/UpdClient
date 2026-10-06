#include "support/mock_transport.hpp"
#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <updclient/net/http_lite.hpp>

#include <string>

using namespace updclient;
using namespace updclient::net;
using ut::Bytes;
using ut::MockScript;

namespace {

Bytes response(const std::string &head, const Bytes &body = {}) {
  Bytes out = ut::bytesOf(head);
  ut::append(out, body);
  return out;
}

HttpRequest request(const std::string &path = "/") {
  return HttpRequest("xell", path);
}

const std::string kOkHead = "HTTP/1.0 200 OK\r\n\r\n";

} // namespace

TEST(HttpHead, ParsesStatusAndHeaders) {
  auto head = parseHttpHead("HTTP/1.1 200 OK\r\nContent-Length: 12\r\nServer: XeLL\r\n\r\n");
  REQUIRE_OK(head);
  CHECK_EQ(head->statusCode, 200);
  CHECK_EQ(head->version, std::string("HTTP/1.1"));
  CHECK_EQ(head->reason, std::string("OK"));
  REQUIRE(head->contentLength.has_value());
  CHECK_EQ(*head->contentLength, size_t{12});
  REQUIRE(head->header("server") != nullptr);
  CHECK_EQ(*head->header("SERVER"), std::string("XeLL"));
  CHECK(head->isSuccess());
}

TEST(HttpHead, AcceptsBareLfLineEndings) {
  auto head = parseHttpHead("HTTP/1.0 200 OK\nContent-Length: 3\nX-A: b\n\n");
  REQUIRE_OK(head);
  CHECK_EQ(head->statusCode, 200);
  CHECK_EQ(*head->contentLength, size_t{3});
  CHECK_EQ(*head->header("x-a"), std::string("b"));
}

TEST(HttpHead, AcceptsMixedLineEndings) {
  auto head = parseHttpHead("HTTP/1.0 200 OK\r\nA: 1\nB: 2\r\n\r\n");
  REQUIRE_OK(head);
  CHECK_EQ(*head->header("a"), std::string("1"));
  CHECK_EQ(*head->header("b"), std::string("2"));
}

TEST(HttpHead, DuplicateHeadersAreJoined) {
  auto head = parseHttpHead("HTTP/1.0 200 OK\r\nSet-Cookie: a\r\nset-cookie: b\r\n\r\n");
  REQUIRE_OK(head);
  CHECK_EQ(*head->header("Set-Cookie"), std::string("a, b"));
}

TEST(HttpHead, FoldedHeaderContinuesPreviousValue) {
  auto head = parseHttpHead("HTTP/1.0 200 OK\r\nX-Long: first\r\n second\r\n\r\n");
  REQUIRE_OK(head);
  CHECK_EQ(*head->header("x-long"), std::string("first second"));
}

TEST(HttpHead, ReasonPhraseMayBeMissing) {
  auto head = parseHttpHead("HTTP/1.0 204\r\n\r\n");
  REQUIRE_OK(head);
  CHECK_EQ(head->statusCode, 204);
  CHECK(head->reason.empty());
}

TEST(HttpHead, RejectsNonHttp) {
  CHECK_ERR(parseHttpHead("SSH-2.0-OpenSSH\r\n\r\n"), ErrorCode::Protocol);
  CHECK_ERR(parseHttpHead(""), ErrorCode::Protocol);
}

TEST(HttpHead, RejectsMalformedStatusLines) {
  CHECK_ERR(parseHttpHead("HTTP/1 200 OK\r\n\r\n"), ErrorCode::Protocol);
  CHECK_ERR(parseHttpHead("HTTP/1.1 20 OK\r\n\r\n"), ErrorCode::Protocol);
  CHECK_ERR(parseHttpHead("HTTP/1.1 2000 OK\r\n\r\n"), ErrorCode::Protocol);
  CHECK_ERR(parseHttpHead("HTTP/1.1 abc OK\r\n\r\n"), ErrorCode::Protocol);
  CHECK_ERR(parseHttpHead("HTTP/1.1 099 Low\r\n\r\n"), ErrorCode::Protocol);
}

TEST(HttpHead, ContentLengthValidation) {
  CHECK_ERR(parseHttpHead("HTTP/1.0 200 OK\r\nContent-Length: abc\r\n\r\n"), ErrorCode::Protocol);
  CHECK_ERR(parseHttpHead("HTTP/1.0 200 OK\r\nContent-Length: -1\r\n\r\n"), ErrorCode::Protocol);
  CHECK_ERR(parseHttpHead("HTTP/1.0 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n"),
            ErrorCode::Protocol);
  auto same = parseHttpHead("HTTP/1.0 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n");
  REQUIRE_OK(same);
  CHECK_EQ(*same->contentLength, size_t{5});
}

TEST(HttpHead, StatusClasses) {
  auto notFound = parseHttpHead("HTTP/1.0 404 Not Found\r\n\r\n");
  REQUIRE_OK(notFound);
  CHECK(!notFound->isSuccess());
  CHECK(notFound->mayHaveBody());
  auto noContent = parseHttpHead("HTTP/1.0 204 No Content\r\n\r\n");
  REQUIRE_OK(noContent);
  CHECK(noContent->isSuccess());
  CHECK(!noContent->mayHaveBody());
}

TEST(HttpRequest, SendsExactRequestBytes) {
  auto script = MockScript::create();
  script->expectWrite("GET /x HTTP/1.0\r\nHost: xell\r\nUser-Agent: UpdClient/2.0\r\nConnection: close\r\n\r\n");
  script->reply(kOkHead);
  auto t = script->transport();
  auto r = httpGet(*t, request("/x"));
  REQUIRE_OK(r);
  CHECK_EQ(script->problems(), std::string());
}

TEST(HttpRequest, ExtraHeadersAndCustomUserAgent) {
  auto script = MockScript::create();
  script->expectWrite("GET / HTTP/1.0\r\nHost: h:8080\r\nUser-Agent: probe\r\nConnection: close\r\nX-One: 1\r\n\r\n");
  script->reply(kOkHead);
  auto t = script->transport();
  HttpRequest req("h:8080", "/");
  req.userAgent = "probe";
  req.extraHeaders["X-One"] = "1";
  REQUIRE_OK(httpGet(*t, req));
  CHECK_EQ(script->problems(), std::string());
}

TEST(HttpRequest, EmptyUserAgentOmitsTheHeader) {
  auto script = MockScript::create();
  script->reply(kOkHead);
  auto t = script->transport();
  HttpRequest req("h", "/");
  req.userAgent.clear();
  REQUIRE_OK(httpGet(*t, req));
  CHECK(script->writtenText().find("User-Agent") == std::string::npos);
}

TEST(HttpRequest, WorksWithShortWrites) {
  auto script = MockScript::create();
  script->setMaxWrite(5);
  script->reply(kOkHead);
  auto t = script->transport();
  REQUIRE_OK(httpGet(*t, request("/long/path/name")));
  CHECK(script->writtenText().rfind("GET /long/path/name HTTP/1.0\r\n", 0) == 0);
}

TEST(HttpRequest, InvalidRequestsAreRejectedBeforeAnythingIsSent) {
  const auto rejects = [](HttpRequest req) {
    auto script = MockScript::create();
    script->reply("HTTP/1.0 200 OK\r\n\r\n");
    auto t = script->transport();
    const auto r = httpGet(*t, req);
    CHECK_ERR(r, ErrorCode::InvalidArgument);
    CHECK_EQ(script->writeCalls(), size_t{0});
  };
  rejects(HttpRequest("", "/"));
  rejects(HttpRequest("ho st", "/"));
  rejects(HttpRequest("host\r\nX: y", "/"));
  rejects(HttpRequest("host", ""));
  rejects(HttpRequest("host", "no-slash"));
  rejects(HttpRequest("host", "/a b"));
  rejects(HttpRequest("host", "/a\r\nInjected: 1"));
  HttpRequest badAgent("host", "/");
  badAgent.userAgent = "a\r\nb";
  rejects(badAgent);
  HttpRequest badHeaderName("host", "/");
  badHeaderName.extraHeaders["Bad:Name"] = "v";
  rejects(badHeaderName);
  HttpRequest badHeaderValue("host", "/");
  badHeaderValue.extraHeaders["X"] = "v\nw";
  rejects(badHeaderValue);
}

TEST(HttpBody, ContentLengthBody) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\nContent-Length: 11\r\n\r\n", ut::bytesOf("hello world")));
  auto t = script->transport();
  auto r = httpGet(*t, request());
  REQUIRE_OK(r);
  CHECK_EQ(ut::textOf(r->body), std::string("hello world"));
  CHECK_EQ(r->head.statusCode, 200);
}

TEST(HttpBody, ContentLengthBodySplitAcrossManyReads) {
  const Bytes body = ut::patternBytes(5000, 2);
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\nContent-Length: 5000\r\n\r\n").replyChunked(body, 77);
  auto t = script->transport();
  auto r = httpGet(*t, request());
  REQUIRE_OK(r);
  CHECK_EQ(r->body, body);
}

TEST(HttpBody, BodyBytesArrivingWithTheHeadersAreKept) {
  const Bytes body = ut::patternBytes(300, 11);
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\nContent-Length: 300\r\n\r\n", body));
  auto t = script->transport();
  auto r = httpGet(*t, request());
  REQUIRE_OK(r);
  CHECK_EQ(r->body, body);
}

TEST(HttpBody, HeadersSplitAcrossReadsByteByByte) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\nContent-Length: 4\r\n\r\nabcd")).setMaxRead(1);
  auto t = script->transport();
  auto r = httpGet(*t, request());
  REQUIRE_OK(r);
  CHECK_EQ(ut::textOf(r->body), std::string("abcd"));
}

TEST(HttpBody, EmptyContentLengthBody) {
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n");
  auto t = script->transport();
  auto r = httpGet(*t, request());
  REQUIRE_OK(r);
  CHECK(r->body.empty());
  CHECK_EQ(script->readCalls(), size_t{1});
}

TEST(HttpBody, ExtraBytesBeyondContentLengthAreDropped) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\n", ut::bytesOf("abcXYZ")));
  auto t = script->transport();
  auto r = httpGet(*t, request());
  REQUIRE_OK(r);
  CHECK_EQ(ut::textOf(r->body), std::string("abc"));
}

TEST(HttpBody, TruncatedContentLengthBodyIsDisconnected) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\nContent-Length: 100\r\n\r\n", ut::patternBytes(60)));
  auto t = script->transport();
  const auto r = httpGet(*t, request());
  REQUIRE_ERR(r, ErrorCode::Disconnected);
  CHECK(r.error().message.find("60 of 100") != std::string::npos);
}

TEST(HttpBody, WithoutContentLengthTheBodyRunsToEof) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\n\r\n", ut::bytesOf("until the connection closes")));
  auto t = script->transport();
  auto r = httpGet(*t, request());
  REQUIRE_OK(r);
  CHECK_EQ(ut::textOf(r->body), std::string("until the connection closes"));
}

// Regression: with no Content-Length the final, shorter read used to be dropped.
TEST(HttpBody, NoContentLengthKeepsPartialFinalChunk) {
  for (size_t size : {size_t{40000}, size_t{16384 + 1}, size_t{16384}, size_t{16383}, size_t{1}, size_t{65536 + 7}}) {
    const Bytes body = ut::patternBytes(size, static_cast<uint32_t>(size));
    auto script = MockScript::create();
    script->reply("HTTP/1.0 200 OK\r\n\r\n").replyChunked(body, 16384);
    auto t = script->transport();
    HttpGetOptions options = HttpGetOptions::unbounded();
    auto r = httpGet(*t, request(), options);
    REQUIRE_OK(r);
    CHECK_EQ(r->body.size(), size);
    CHECK_EQ(r->body, body);
  }
}

TEST(HttpBody, NoContentLengthPartialFinalChunkWithHeadInFirstRead) {
  for (size_t size : {size_t{40000}, size_t{16384 + 1}}) {
    const Bytes body = ut::patternBytes(size, 3);
    auto script = MockScript::create();
    script->reply(response("HTTP/1.0 200 OK\r\n\r\n", body)).setMaxRead(16384);
    auto t = script->transport();
    auto r = httpGet(*t, request(), HttpGetOptions::unbounded());
    REQUIRE_OK(r);
    CHECK_EQ(r->body, body);
  }
}

TEST(HttpBody, NoContentLengthTinyReads) {
  const Bytes body = ut::patternBytes(1000, 6);
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\n\r\n", body)).setMaxRead(13);
  auto t = script->transport();
  auto r = httpGet(*t, request(), HttpGetOptions::unbounded());
  REQUIRE_OK(r);
  CHECK_EQ(r->body, body);
}

TEST(HttpBody, DefaultBodyLimitAppliesWithoutContentLength) {
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\n\r\n").replyChunked(ut::patternBytes(kHttpDefaultMaxBodyBytes + 10), 32768);
  auto t = script->transport();
  CHECK_ERR(httpGet(*t, request()), ErrorCode::LimitExceeded);
}

TEST(HttpBody, ContentLengthOverLimitIsRejectedBeforeReadingTheBody) {
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\nContent-Length: 5000\r\n\r\n");
  script->reply(ut::patternBytes(5000));
  auto t = script->transport();
  HttpGetOptions options;
  options.maxBodyBytes = 1000;
  CHECK_ERR(httpGet(*t, request(), options), ErrorCode::LimitExceeded);
  CHECK_EQ(script->unreadBytes(), size_t{5000});
}

TEST(HttpBody, ExactlyAtTheLimitIsAccepted) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\nContent-Length: 1000\r\n\r\n", ut::patternBytes(1000)));
  auto t = script->transport();
  HttpGetOptions options;
  options.maxBodyBytes = 1000;
  auto r = httpGet(*t, request(), options);
  REQUIRE_OK(r);
  CHECK_EQ(r->body.size(), size_t{1000});
}

TEST(HttpBody, TimeoutMidBodyPropagates) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\nContent-Length: 100\r\n\r\n", ut::patternBytes(10))).replyTimeout();
  auto t = script->transport();
  CHECK_ERR(httpGet(*t, request()), ErrorCode::Timeout);
}

TEST(HttpBody, TimeoutMidBodyWithoutContentLengthPropagates) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\n\r\n", ut::patternBytes(10))).replyTimeout();
  auto t = script->transport();
  CHECK_ERR(httpGet(*t, request()), ErrorCode::Timeout);
}

TEST(HttpBody, NoBodyForNoContentStatus) {
  auto script = MockScript::create();
  script->reply("HTTP/1.0 204 No Content\r\n\r\n");
  script->reply("this must not be delivered");
  auto t = script->transport();
  auto r = httpGet(*t, request());
  REQUIRE_OK(r);
  CHECK(r->body.empty());
}

TEST(HttpBody, InterimResponsesAreSkipped) {
  auto script = MockScript::create();
  script->reply("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  auto t = script->transport();
  auto r = httpGet(*t, request());
  REQUIRE_OK(r);
  CHECK_EQ(r->head.statusCode, 200);
  CHECK_EQ(ut::textOf(r->body), std::string("ok"));
}

TEST(HttpHeaders, LfOnlyHeadersWithBody) {
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\nContent-Length: 5\n\nhello");
  auto t = script->transport();
  auto r = httpGet(*t, request());
  REQUIRE_OK(r);
  CHECK_EQ(ut::textOf(r->body), std::string("hello"));
}

TEST(HttpHeaders, LfOnlyHeadersWithoutContentLength) {
  const Bytes body = ut::patternBytes(20000, 4);
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\nServer: XeLL\n\n", body));
  auto t = script->transport();
  auto r = httpGet(*t, request(), HttpGetOptions::unbounded());
  REQUIRE_OK(r);
  CHECK_EQ(r->body, body);
}

TEST(HttpHeaders, HeaderTerminatorSplitAcrossReads) {
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r").reply("\nhi");
  auto t = script->transport();
  auto r = httpGet(*t, request());
  REQUIRE_OK(r);
  CHECK_EQ(ut::textOf(r->body), std::string("hi"));
}

TEST(HttpHeaders, HeaderBlockLargerThanCapIsRejected) {
  auto script = MockScript::create();
  std::string head = "HTTP/1.0 200 OK\r\n";
  while (head.size() < 40 * 1024) head += "X-Pad: " + std::string(100, 'a') + "\r\n";
  head += "\r\n";
  script->reply(head);
  auto t = script->transport();
  const auto r = httpGet(*t, request());
  REQUIRE_ERR(r, ErrorCode::LimitExceeded);
  CHECK(r.error().message.find("headers") != std::string::npos);
}

TEST(HttpHeaders, HeadersThatNeverEndAreCapped) {
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\n");
  script->replyChunked(ut::bytesOf(std::string(100 * 1024, 'a')), 4096);
  auto t = script->transport();
  CHECK_ERR(httpGet(*t, request()), ErrorCode::LimitExceeded);
}

TEST(HttpHeaders, CustomHeaderCap) {
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\nX-Pad: " + std::string(300, 'a') + "\r\n\r\n");
  auto t = script->transport();
  HttpGetOptions options;
  options.maxHeaderBytes = 128;
  CHECK_ERR(httpGet(*t, request(), options), ErrorCode::LimitExceeded);
}

TEST(HttpHeaders, HeadersJustUnderTheCapAreAccepted) {
  auto script = MockScript::create();
  std::string head = "HTTP/1.0 200 OK\r\nX-Pad: " + std::string(100, 'a') + "\r\nContent-Length: 0\r\n\r\n";
  script->reply(head);
  auto t = script->transport();
  HttpGetOptions options;
  options.maxHeaderBytes = head.size();
  CHECK_OK(httpGet(*t, request(), options));
}

TEST(HttpHeaders, ConnectionClosedBeforeAnyResponse) {
  auto script = MockScript::create();
  auto t = script->transport();
  const auto r = httpGet(*t, request());
  REQUIRE_ERR(r, ErrorCode::Disconnected);
}

TEST(HttpHeaders, ConnectionClosedInsideHeaders) {
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\nContent-Le");
  auto t = script->transport();
  CHECK_ERR(httpGet(*t, request()), ErrorCode::Disconnected);
}

TEST(HttpHeaders, TimeoutBeforeHeaders) {
  auto script = MockScript::create();
  script->replyTimeout();
  auto t = script->transport();
  CHECK_ERR(httpGet(*t, request()), ErrorCode::Timeout);
}

TEST(HttpHeaders, GarbageResponseIsAProtocolError) {
  auto script = MockScript::create();
  script->reply("\x01\x02garbage\r\n\r\n");
  auto t = script->transport();
  CHECK_ERR(httpGet(*t, request()), ErrorCode::Protocol);
}

TEST(HttpStatus, Non200IsAProtocolErrorByDefault) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 404 Not Found\r\nContent-Length: 4\r\n\r\nnope"));
  auto t = script->transport();
  const auto r = httpGet(*t, request("/missing"));
  REQUIRE_ERR(r, ErrorCode::Protocol);
  CHECK(r.error().message.find("404") != std::string::npos);
  CHECK(r.error().message.find("/missing") != std::string::npos);
}

TEST(HttpStatus, ServerErrorAndRedirectAreErrors) {
  for (const char *status : {"500 Internal Server Error", "302 Found", "403 Forbidden"}) {
    auto script = MockScript::create();
    script->reply(std::string("HTTP/1.0 ") + status + "\r\n\r\n");
    auto t = script->transport();
    CHECK_ERR(httpGet(*t, request()), ErrorCode::Protocol);
  }
}

TEST(HttpStatus, RequireSuccessCanBeDisabled) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 404 Not Found\r\nContent-Length: 4\r\n\r\nnope"));
  auto t = script->transport();
  HttpGetOptions options;
  options.requireSuccess = false;
  auto r = httpGet(*t, request(), options);
  REQUIRE_OK(r);
  CHECK_EQ(r->head.statusCode, 404);
  CHECK_EQ(ut::textOf(r->body), std::string("nope"));
}

TEST(HttpTransferEncoding, ChunkedIsRejectedAsUnsupported) {
  auto script = MockScript::create();
  script->reply("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");
  auto t = script->transport();
  const auto r = httpGet(*t, request());
  REQUIRE_ERR(r, ErrorCode::Unsupported);
  CHECK(r.error().message.find("chunked") != std::string::npos);
}

TEST(HttpTransferEncoding, ChunkedHeaderIsMatchedCaseInsensitively) {
  auto script = MockScript::create();
  script->reply("HTTP/1.1 200 OK\r\ntransfer-encoding: Chunked\r\n\r\n0\r\n\r\n");
  auto t = script->transport();
  CHECK_ERR(httpGet(*t, request()), ErrorCode::Unsupported);
}

TEST(HttpTransferEncoding, ChunkedWithContentLengthIsStillUnsupported) {
  auto script = MockScript::create();
  script->reply("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\nhello");
  auto t = script->transport();
  CHECK_ERR(httpGet(*t, request()), ErrorCode::Unsupported);
}

TEST(HttpTransferEncoding, IdentityIsAccepted) {
  auto script = MockScript::create();
  script->reply("HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\nContent-Length: 2\r\n\r\nok");
  auto t = script->transport();
  auto r = httpGet(*t, request());
  REQUIRE_OK(r);
  CHECK_EQ(ut::textOf(r->body), std::string("ok"));
}

TEST(HttpTransferEncoding, ChunkedDoesNotAffectBodylessResponses) {
  auto script = MockScript::create();
  script->reply("HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked\r\n\r\n");
  auto t = script->transport();
  CHECK_OK(httpGet(*t, request()));
}

TEST(HttpExchange, ReadBodyPrefixStopsWithoutWaitingForEof) {
  const Bytes full = ut::patternBytes(100);
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\n\r\n", full));
  script->replyTimeout();
  auto t = script->transport();
  HttpExchange exchange(*t);
  REQUIRE_OK(exchange.sendGet(request()));
  auto head = exchange.readHead();
  REQUIRE_OK(head);
  auto prefix = exchange.readBodyPrefix(*head, 40);
  REQUIRE_OK(prefix);
  CHECK_EQ(prefix->size(), size_t{40});
  CHECK_EQ(*prefix, Bytes(full.begin(), full.begin() + 40));
}

TEST(HttpExchange, ReadBodyPrefixReturnsShortBodyWhole) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nabcde"));
  auto t = script->transport();
  HttpExchange exchange(*t);
  REQUIRE_OK(exchange.sendGet(request()));
  auto head = exchange.readHead();
  REQUIRE_OK(head);
  auto prefix = exchange.readBodyPrefix(*head, 1000);
  REQUIRE_OK(prefix);
  CHECK_EQ(ut::textOf(*prefix), std::string("abcde"));
}

TEST(HttpExchange, ReadBodyPrefixOnEofTerminatedBody) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\n\r\n", ut::bytesOf("short page")));
  auto t = script->transport();
  HttpExchange exchange(*t);
  REQUIRE_OK(exchange.sendGet(request()));
  auto head = exchange.readHead();
  REQUIRE_OK(head);
  auto prefix = exchange.readBodyPrefix(*head, 16 * 1024);
  REQUIRE_OK(prefix);
  CHECK_EQ(ut::textOf(*prefix), std::string("short page"));
}

TEST(HttpExchange, ProgressReportsReceivedAndTotal) {
  const Bytes body = ut::patternBytes(1000, 2);
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\nContent-Length: 1000\r\n\r\n").replyChunked(body, 400);
  auto t = script->transport();
  std::vector<std::pair<size_t, size_t>> calls;
  auto r = httpGetStream(
      *t, request(), [](std::span<const uint8_t>) -> Result<void> { return {}; },
      [&](size_t received, size_t total) { calls.emplace_back(received, total); });
  REQUIRE_OK(r);
  CHECK_EQ(r->bytes, size_t{1000});
  REQUIRE_EQ(calls.size(), size_t{3});
  CHECK_EQ(calls[0], (std::pair<size_t, size_t>{400, 1000}));
  CHECK_EQ(calls[2], (std::pair<size_t, size_t>{1000, 1000}));
}

TEST(HttpExchange, ProgressTotalIsZeroWithoutContentLength) {
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\n\r\n", ut::patternBytes(100)));
  auto t = script->transport();
  size_t lastTotal = 99;
  auto r = httpGetStream(
      *t, request(), [](std::span<const uint8_t>) -> Result<void> { return {}; },
      [&](size_t, size_t total) { lastTotal = total; });
  REQUIRE_OK(r);
  CHECK_EQ(lastTotal, size_t{0});
}

TEST(HttpExchange, SinkErrorAbortsTheDownload) {
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\nContent-Length: 1000\r\n\r\n").replyChunked(ut::patternBytes(1000), 100);
  auto t = script->transport();
  int calls = 0;
  const auto r = httpGetStream(*t, request(), [&](std::span<const uint8_t>) -> Result<void> {
    if (++calls == 2) return fail(ErrorCode::Io, "disk full");
    return {};
  });
  REQUIRE_ERR(r, ErrorCode::Io);
  CHECK_EQ(calls, 2);
}

TEST(HttpGetToFile, WritesTheWholeBodyAndLeavesNoTemporaryFiles) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const Bytes body = ut::patternBytes(40000, 12);
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\n\r\n").replyChunked(body, 16384);
  auto t = script->transport();
  const auto dest = dir.file("flash.bin");
  auto r = httpGetToFile(*t, request("/FLASH"), dest);
  REQUIRE_OK(r);
  CHECK_EQ(r->bytes, size_t{40000});
  auto onDisk = ut::readFile(dest);
  REQUIRE(onDisk.has_value());
  CHECK_EQ(*onDisk, body);
  CHECK_EQ(dir.entries(), std::vector<std::string>{"flash.bin"});
}

TEST(HttpGetToFile, TruncatedDownloadLeavesNoFile) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\nContent-Length: 5000\r\n\r\n", ut::patternBytes(1200)));
  auto t = script->transport();
  const auto dest = dir.file("flash.bin");
  CHECK_ERR(httpGetToFile(*t, request("/FLASH"), dest), ErrorCode::Disconnected);
  CHECK(dir.entries().empty());
}

TEST(HttpGetToFile, FailureKeepsAnExistingDestinationIntact) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const auto dest = dir.file("flash.bin");
  const Bytes previous = ut::bytesOf("previous good dump");
  REQUIRE(ut::writeFile(dest, previous));

  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\nContent-Length: 5000\r\n\r\n", ut::patternBytes(10)));
  script->replyTimeout();
  auto t = script->transport();
  CHECK_ERR(httpGetToFile(*t, request("/FLASH"), dest), ErrorCode::Timeout);
  auto onDisk = ut::readFile(dest);
  REQUIRE(onDisk.has_value());
  CHECK_EQ(*onDisk, previous);
  CHECK_EQ(dir.entries(), std::vector<std::string>{"flash.bin"});
}

TEST(HttpGetToFile, HttpErrorCreatesNothing) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto script = MockScript::create();
  script->reply("HTTP/1.0 404 Not Found\r\n\r\nnope");
  auto t = script->transport();
  CHECK_ERR(httpGetToFile(*t, request("/FLASH"), dir.file("out.bin")), ErrorCode::Protocol);
  CHECK(dir.entries().empty());
}

TEST(HttpGetToFile, ReplacesAnExistingDestinationOnSuccess) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  const auto dest = dir.file("out.bin");
  REQUIRE(ut::writeFile(dest, ut::bytesOf("old old old old old")));
  auto script = MockScript::create();
  script->reply(response("HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\nnew"));
  auto t = script->transport();
  REQUIRE_OK(httpGetToFile(*t, request("/FLASH"), dest));
  auto onDisk = ut::readFile(dest);
  REQUIRE(onDisk.has_value());
  CHECK_EQ(ut::textOf(*onDisk), std::string("new"));
}

TEST(HttpGetToFile, RejectsEmptyOrDirectoryLikePaths) {
  auto script = MockScript::create();
  auto t = script->transport();
  CHECK_ERR(httpGetToFile(*t, request(), std::filesystem::path()), ErrorCode::InvalidArgument);
  CHECK_EQ(script->writeCalls(), size_t{0});
}

TEST(HttpGetToFile, UnwritableDestinationDirectoryIsAnIoError) {
  ut::TempDir dir;
  REQUIRE(dir.ok());
  auto script = MockScript::create();
  script->reply("HTTP/1.0 200 OK\r\n\r\nbody");
  auto t = script->transport();
  CHECK_ERR(httpGetToFile(*t, request(), dir.file("missing-subdir/out.bin")), ErrorCode::Io);
}
