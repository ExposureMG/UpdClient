#include <net/http_lite.hpp>

#include <core/path.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <system_error>

namespace updclient::net {

namespace {

constexpr size_t kHeadReadChunk = 8 * 1024;
constexpr size_t kBodyReadChunk = 32 * 1024;
constexpr size_t kMaxReserve = 1024 * 1024;

bool isSpace(char c) noexcept {
  return c == ' ' || c == '\t';
}

bool isDigit(char c) noexcept {
  return c >= '0' && c <= '9';
}

char lowerAscii(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
  return text;
}

std::string toLower(std::string_view text) {
  std::string out(text);
  for (char &c : out) c = lowerAscii(c);
  return out;
}

bool hasControlChars(std::string_view text, bool allowSpace) {
  for (char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) return true;
    if (!allowSpace && c == ' ') return true;
  }
  return false;
}

// Index one past the blank line that ends the header block, or nullopt.
// Handles CRLF CRLF, LF LF and mixed endings.
std::optional<size_t> findHeadEnd(std::string_view data, size_t from) {
  for (size_t i = from; i < data.size(); ++i) {
    if (data[i] != '\n') continue;
    if (i + 1 < data.size() && data[i + 1] == '\n') return i + 2;
    if (i + 2 < data.size() && data[i + 1] == '\r' && data[i + 2] == '\n') return i + 3;
  }
  return std::nullopt;
}

std::string_view nextLine(std::string_view &text) {
  const auto newline = text.find('\n');
  std::string_view line = text.substr(0, newline);
  text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  return line;
}

Result<size_t> parseSize(std::string_view text) {
  size_t value = 0;
  if (text.empty()) return fail(ErrorCode::Protocol, "empty number");
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    return fail(ErrorCode::Protocol, "invalid number '" + std::string(text) + "'");
  }
  return value;
}

Result<std::optional<size_t>> parseContentLength(const std::string &value) {
  std::optional<size_t> length;
  std::string_view rest = value;
  for (;;) {
    const auto comma = rest.find(',');
    auto parsed = parseSize(trim(rest.substr(0, comma)));
    if (!parsed) return fail(ErrorCode::Protocol, "invalid Content-Length '" + value + "'");
    if (length && *length != *parsed) {
      return fail(ErrorCode::Protocol, "conflicting Content-Length values '" + value + "'");
    }
    length = *parsed;
    if (comma == std::string_view::npos) break;
    rest.remove_prefix(comma + 1);
  }
  return length;
}

Result<void> parseStatusLine(std::string_view line, HttpResponseHead &head) {
  if (line.compare(0, 5, "HTTP/") != 0) return fail(ErrorCode::Protocol, "response is not HTTP (no status line)");

  size_t pos = 0;
  while (pos < line.size() && !isSpace(line[pos])) ++pos;
  const std::string_view version = line.substr(0, pos);
  if (version.size() != 8 || !isDigit(version[5]) || version[6] != '.' || !isDigit(version[7])) {
    return fail(ErrorCode::Protocol, "malformed HTTP version '" + std::string(version) + "'");
  }
  head.version = std::string(version);

  while (pos < line.size() && isSpace(line[pos])) ++pos;
  const std::string_view rest = line.substr(pos);
  if (rest.size() < 3 || !isDigit(rest[0]) || !isDigit(rest[1]) || !isDigit(rest[2]) ||
      (rest.size() > 3 && !isSpace(rest[3]))) {
    return fail(ErrorCode::Protocol, "malformed HTTP status code");
  }
  head.statusCode = (rest[0] - '0') * 100 + (rest[1] - '0') * 10 + (rest[2] - '0');
  if (head.statusCode < 100) return fail(ErrorCode::Protocol, "invalid HTTP status code");
  head.reason = std::string(trim(rest.substr(3)));
  return {};
}

Result<void> validateRequest(const HttpRequest &request) {
  if (request.host.empty() || hasControlChars(request.host, false)) {
    return fail(ErrorCode::InvalidArgument, "invalid HTTP host");
  }
  if (request.path.empty() || request.path.front() != '/' || hasControlChars(request.path, false)) {
    return fail(ErrorCode::InvalidArgument, "invalid HTTP request path '" + request.path + "'");
  }
  if (hasControlChars(request.userAgent, true)) {
    return fail(ErrorCode::InvalidArgument, "invalid HTTP User-Agent");
  }
  for (const auto &[name, value] : request.extraHeaders) {
    if (name.empty() || name.find(':') != std::string::npos || hasControlChars(name, false) ||
        hasControlChars(value, true)) {
      return fail(ErrorCode::InvalidArgument, "invalid HTTP header '" + name + "'");
    }
  }
  return {};
}

Result<void> requireSuccess(const HttpRequest &request, const HttpResponseHead &head) {
  if (head.isSuccess()) return {};
  std::string message = "HTTP GET " + request.path + " failed with status " + std::to_string(head.statusCode);
  if (!head.reason.empty()) message += " " + head.reason;
  return fail(ErrorCode::Protocol, std::move(message));
}

struct TempFileGuard {
  std::filesystem::path path;
  bool armed = true;

  ~TempFileGuard() {
    if (!armed) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

std::filesystem::path makeTempPath(const std::filesystem::path &destination) {
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  std::array<char, 24> suffix{};
  const auto [end, ec] = std::to_chars(suffix.data(), suffix.data() + suffix.size(),
                                       static_cast<unsigned long long>(ticks), 16);
  std::filesystem::path temp = destination;
  temp += ".";
  temp += std::string(suffix.data(), ec == std::errc{} ? end : suffix.data());
  temp += ".part";
  return temp;
}

} // namespace

Result<HttpResponseHead> parseHttpHead(std::string_view raw) {
  HttpResponseHead head;
  const std::string_view statusLine = nextLine(raw);
  if (auto r = parseStatusLine(statusLine, head); !r) return unexpected<Error>(r.error());

  HttpHeaders::iterator last = head.headers.end();
  while (!raw.empty()) {
    const std::string_view line = nextLine(raw);
    if (line.empty()) break;

    if (isSpace(line.front())) {
      if (last != head.headers.end()) {
        last->second += ' ';
        last->second += std::string(trim(line));
      }
      continue;
    }

    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) continue;
    std::string name(trim(line.substr(0, colon)));
    std::string value(trim(line.substr(colon + 1)));
    if (name.empty()) continue;

    auto [it, inserted] = head.headers.try_emplace(std::move(name), value);
    if (!inserted) {
      it->second += ", ";
      it->second += value;
    }
    last = it;
  }

  if (const std::string *value = head.header("content-length")) {
    auto length = parseContentLength(*value);
    if (!length) return unexpected<Error>(length.error());
    head.contentLength = *length;
  }
  return head;
}

HttpExchange::HttpExchange(ITransport &transport, size_t maxHeaderBytes)
    : transport_(transport), maxHeaderBytes_(maxHeaderBytes) {}

Result<void> HttpExchange::sendGet(const HttpRequest &request) {
  if (auto r = validateRequest(request); !r) return r;

  std::string text = "GET " + request.path + " HTTP/1.0\r\nHost: " + request.host + "\r\n";
  if (!request.userAgent.empty()) text += "User-Agent: " + request.userAgent + "\r\n";
  text += "Connection: close\r\n";
  for (const auto &[name, value] : request.extraHeaders) text += name + ": " + value + "\r\n";
  text += "\r\n";

  return transport_.writeAll(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}

Result<HttpResponseHead> HttpExchange::readHead() {
  std::array<uint8_t, kHeadReadChunk> chunk{};
  size_t searchFrom = 0;

  for (;;) {
    if (const auto end = findHeadEnd(buffer_, searchFrom)) {
      if (*end > maxHeaderBytes_) {
        return fail(ErrorCode::LimitExceeded,
                    "HTTP headers exceed " + std::to_string(maxHeaderBytes_) + " bytes");
      }
      auto head = parseHttpHead(std::string_view(buffer_).substr(0, *end));
      buffer_.erase(0, *end);
      if (!head) return head;
      // Interim 1xx responses (other than 101) are followed by the real one.
      if (head->statusCode >= 100 && head->statusCode < 200 && head->statusCode != 101) {
        searchFrom = 0;
        continue;
      }
      return head;
    }

    if (buffer_.size() >= maxHeaderBytes_) {
      return fail(ErrorCode::LimitExceeded, "HTTP headers exceed " + std::to_string(maxHeaderBytes_) + " bytes");
    }
    searchFrom = buffer_.size() >= 2 ? buffer_.size() - 2 : 0;

    auto n = transport_.readSome(std::span<uint8_t>(chunk));
    if (!n) return unexpected<Error>(n.error());
    if (*n == 0) {
      if (buffer_.empty()) return fail(ErrorCode::Disconnected, "connection closed before any HTTP response");
      return fail(ErrorCode::Disconnected, "connection closed inside HTTP headers");
    }
    buffer_.append(reinterpret_cast<const char *>(chunk.data()), *n);
  }
}

Result<size_t> HttpExchange::pump(const HttpResponseHead &head, const HttpBodySink &sink,
                                  const HttpProgress &progress, size_t limit, bool truncateAtLimit) {
  if (!head.mayHaveBody()) return size_t{0};

  if (const std::string *encoding = head.header("transfer-encoding")) {
    if (toLower(trim(*encoding)) != "identity") {
      return fail(ErrorCode::Unsupported, "Transfer-Encoding '" + *encoding + "' is not supported");
    }
  }

  const bool known = head.contentLength.has_value();
  const size_t total = known ? *head.contentLength : 0;
  if (known && total > limit && !truncateAtLimit) {
    return fail(ErrorCode::LimitExceeded, "HTTP body of " + std::to_string(total) +
                                              " bytes exceeds limit of " + std::to_string(limit));
  }

  size_t received = 0;
  bool done = known && total == 0;

  auto deliver = [&](std::span<const uint8_t> data) -> Result<void> {
    if (known && data.size() > total - received) data = data.first(total - received);
    if (data.size() > limit - received) {
      if (!truncateAtLimit) {
        return fail(ErrorCode::LimitExceeded, "HTTP body exceeds limit of " + std::to_string(limit) + " bytes");
      }
      data = data.first(limit - received);
      done = true;
    }
    if (!data.empty()) {
      if (sink) {
        if (auto r = sink(data); !r) return r;
      }
      received += data.size();
      if (progress) progress(received, total);
    }
    if (known && received >= total) done = true;
    return {};
  };

  if (!done && !buffer_.empty()) {
    const std::string pending = std::move(buffer_);
    buffer_.clear();
    if (auto r = deliver(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(pending.data()),
                                                  pending.size()));
        !r) {
      return unexpected<Error>(r.error());
    }
  }

  std::vector<uint8_t> chunk(kBodyReadChunk);
  while (!done) {
    auto n = transport_.readSome(std::span<uint8_t>(chunk));
    if (!n) return unexpected<Error>(n.error());
    if (*n == 0) {
      if (known) {
        return fail(ErrorCode::Disconnected, "HTTP body truncated after " + std::to_string(received) +
                                                 " of " + std::to_string(total) + " bytes");
      }
      break;
    }
    if (auto r = deliver(std::span<const uint8_t>(chunk.data(), *n)); !r) return unexpected<Error>(r.error());
  }
  return received;
}

Result<size_t> HttpExchange::readBody(const HttpResponseHead &head, const HttpBodySink &sink,
                                      const HttpProgress &progress, size_t maxBodyBytes) {
  return pump(head, sink, progress, maxBodyBytes, false);
}

Result<std::vector<uint8_t>> HttpExchange::readBodyPrefix(const HttpResponseHead &head, size_t maxBytes) {
  std::vector<uint8_t> out;
  auto n = pump(
      head,
      [&out](std::span<const uint8_t> data) -> Result<void> {
        out.insert(out.end(), data.begin(), data.end());
        return {};
      },
      {}, maxBytes, true);
  if (!n) return unexpected<Error>(n.error());
  return out;
}

Result<HttpResponse> httpGet(ITransport &transport, const HttpRequest &request, const HttpGetOptions &options) {
  HttpExchange exchange(transport, options.maxHeaderBytes);
  if (auto r = exchange.sendGet(request); !r) return unexpected<Error>(r.error());

  auto head = exchange.readHead();
  if (!head) return unexpected<Error>(head.error());
  if (options.requireSuccess) {
    if (auto r = requireSuccess(request, *head); !r) return unexpected<Error>(r.error());
  }

  HttpResponse response;
  response.head = std::move(*head);
  if (response.head.contentLength) {
    response.body.reserve(std::min({*response.head.contentLength, options.maxBodyBytes, kMaxReserve}));
  }
  auto body = exchange.readBody(
      response.head,
      [&response](std::span<const uint8_t> data) -> Result<void> {
        response.body.insert(response.body.end(), data.begin(), data.end());
        return {};
      },
      {}, options.maxBodyBytes);
  if (!body) return unexpected<Error>(body.error());
  return response;
}

Result<HttpStreamResult> httpGetStream(ITransport &transport, const HttpRequest &request,
                                       const HttpBodySink &sink, const HttpProgress &progress,
                                       const HttpGetOptions &options) {
  HttpExchange exchange(transport, options.maxHeaderBytes);
  if (auto r = exchange.sendGet(request); !r) return unexpected<Error>(r.error());

  auto head = exchange.readHead();
  if (!head) return unexpected<Error>(head.error());
  if (options.requireSuccess) {
    if (auto r = requireSuccess(request, *head); !r) return unexpected<Error>(r.error());
  }

  auto bytes = exchange.readBody(*head, sink, progress, options.maxBodyBytes);
  if (!bytes) return unexpected<Error>(bytes.error());
  return HttpStreamResult{std::move(*head), *bytes};
}

Result<HttpStreamResult> httpGetToFile(ITransport &transport, const HttpRequest &request,
                                       const std::filesystem::path &destination, const HttpProgress &progress,
                                       const HttpGetOptions &options) {
  if (destination.empty() || destination.filename().empty()) {
    return fail(ErrorCode::InvalidArgument, "invalid output path '" + pathToUtf8(destination) + "'");
  }

  HttpExchange exchange(transport, options.maxHeaderBytes);
  if (auto r = exchange.sendGet(request); !r) return unexpected<Error>(r.error());

  auto head = exchange.readHead();
  if (!head) return unexpected<Error>(head.error());
  if (options.requireSuccess) {
    if (auto r = requireSuccess(request, *head); !r) return unexpected<Error>(r.error());
  }

  TempFileGuard guard{makeTempPath(destination)};
  std::ofstream out(guard.path, std::ios::binary | std::ios::trunc);
  if (!out) return fail(ErrorCode::Io, "cannot create '" + pathToUtf8(guard.path) + "'");

  auto bytes = exchange.readBody(
      *head,
      [&out, &guard](std::span<const uint8_t> data) -> Result<void> {
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) return fail(ErrorCode::Io, "write to '" + pathToUtf8(guard.path) + "' failed");
        return {};
      },
      progress, options.maxBodyBytes);
  if (!bytes) return unexpected<Error>(bytes.error());

  out.flush();
  out.close();
  if (out.fail()) return fail(ErrorCode::Io, "flushing '" + pathToUtf8(guard.path) + "' failed");

  std::error_code ec;
  std::filesystem::rename(guard.path, destination, ec);
  if (ec) {
    return fail(ErrorCode::Io, "cannot move download to '" + pathToUtf8(destination) + "': " + ec.message(),
                ec.value());
  }
  guard.armed = false;
  return HttpStreamResult{std::move(*head), *bytes};
}

} // namespace updclient::net
