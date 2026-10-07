#pragma once

#include <core/error.hpp>
#include <core/export.hpp>
#include <net/transport.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Minimal HTTP/1.0 GET client for "Connection: close" devices. It runs on any
// ITransport, sends no keep-alive, and never negotiates chunked encoding. No
// redirects and no TLS. A response that announces Transfer-Encoding other than
// identity is rejected with ErrorCode::Unsupported.
namespace updclient::net {

inline constexpr size_t kHttpDefaultMaxHeaderBytes = 16 * 1024;
inline constexpr size_t kHttpDefaultMaxBodyBytes = 1024 * 1024;
inline constexpr size_t kHttpUnlimited = std::numeric_limits<size_t>::max();

// ASCII case-insensitive ordering, so headers.find("Content-Length") works.
struct HttpHeaderLess {
  using is_transparent = void;

  bool operator()(std::string_view a, std::string_view b) const noexcept {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](char x, char y) { return lower(x) < lower(y); });
  }

private:
  static char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
};

using HttpHeaders = std::map<std::string, std::string, HttpHeaderLess>;

struct HttpRequest {
  HttpRequest() = default;
  HttpRequest(std::string hostName, std::string requestPath)
      : host(std::move(hostName)), path(std::move(requestPath)) {}

  std::string host;
  std::string path = "/";
  std::string userAgent = "UpdClient/2.0";
  HttpHeaders extraHeaders;
};

struct HttpResponseHead {
  int statusCode = 0;
  std::string version;
  std::string reason;
  HttpHeaders headers; // duplicate headers are joined with ", "
  std::optional<size_t> contentLength;

  const std::string *header(std::string_view name) const {
    auto it = headers.find(name);
    return it == headers.end() ? nullptr : &it->second;
  }
  bool isSuccess() const noexcept { return statusCode >= 200 && statusCode < 300; }
  // 1xx, 204 and 304 never carry a body.
  bool mayHaveBody() const noexcept { return statusCode >= 200 && statusCode != 204 && statusCode != 304; }
};

struct HttpResponse {
  HttpResponseHead head;
  std::vector<uint8_t> body;
};

using HttpBodySink = std::function<Result<void>(std::span<const uint8_t> chunk)>;
// total is 0 when the length is unknown.
using HttpProgress = std::function<void(size_t received, size_t total)>;

struct HttpGetOptions {
  size_t maxHeaderBytes = kHttpDefaultMaxHeaderBytes;
  size_t maxBodyBytes = kHttpDefaultMaxBodyBytes;
  // Non-2xx statuses are reported as a Protocol error.
  bool requireSuccess = true;

  static HttpGetOptions unbounded() {
    HttpGetOptions options;
    options.maxBodyBytes = kHttpUnlimited;
    return options;
  }
};

// Parses a status line and headers (raw includes the terminating blank line).
// Accepts CRLF and bare LF line endings.
UPDCLIENT_API Result<HttpResponseHead> parseHttpHead(std::string_view raw);

// One request/response on a transport. Headers are read in buffered chunks, so
// bytes that arrive behind the headers are kept and served by readBody().
class UPDCLIENT_API HttpExchange {
public:
  explicit HttpExchange(ITransport &transport, size_t maxHeaderBytes = kHttpDefaultMaxHeaderBytes);

  Result<void> sendGet(const HttpRequest &request);

  // Header block larger than maxHeaderBytes: LimitExceeded. Peer closing first: Disconnected.
  Result<HttpResponseHead> readHead();

  // Delivers the body to sink and returns the number of bytes delivered. With a
  // Content-Length, ending early is Disconnected and more than maxBodyBytes is
  // LimitExceeded before any byte is read. Without one, the body runs to EOF,
  // including a final partial read.
  Result<size_t> readBody(const HttpResponseHead &head, const HttpBodySink &sink,
                          const HttpProgress &progress = {}, size_t maxBodyBytes = kHttpUnlimited);

  // Up to maxBytes of the body; stops there without error and without waiting for EOF.
  Result<std::vector<uint8_t>> readBodyPrefix(const HttpResponseHead &head, size_t maxBytes);

private:
  Result<size_t> pump(const HttpResponseHead &head, const HttpBodySink &sink, const HttpProgress &progress,
                      size_t limit, bool truncateAtLimit);

  ITransport &transport_;
  size_t maxHeaderBytes_;
  std::string buffer_;
};

// Buffers the whole body in memory (bounded by options.maxBodyBytes).
UPDCLIENT_API Result<HttpResponse> httpGet(ITransport &transport, const HttpRequest &request,
                                           const HttpGetOptions &options = {});

struct HttpStreamResult {
  HttpResponseHead head;
  size_t bytes = 0;
};

UPDCLIENT_API Result<HttpStreamResult> httpGetStream(ITransport &transport, const HttpRequest &request,
                                                     const HttpBodySink &sink,
                                                     const HttpProgress &progress = {},
                                                     const HttpGetOptions &options = HttpGetOptions::unbounded());

// Writes the body to a temporary file next to destination and renames it over
// destination only after the full body was received and flushed. On any failure
// the temporary file is removed and destination is left untouched.
UPDCLIENT_API Result<HttpStreamResult> httpGetToFile(ITransport &transport, const HttpRequest &request,
                                                     const std::filesystem::path &destination,
                                                     const HttpProgress &progress = {},
                                                     const HttpGetOptions &options = HttpGetOptions::unbounded());

} // namespace updclient::net
