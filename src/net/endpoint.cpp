#include <net/endpoint.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace updclient::net {

namespace {

constexpr std::chrono::milliseconds kDefaultTimeout{5000};

std::string toLower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool validScheme(std::string_view scheme) {
  if (scheme.empty() || !std::isalpha(static_cast<unsigned char>(scheme.front()))) return false;
  return std::all_of(scheme.begin(), scheme.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '+' || c == '-' || c == '.';
  });
}

bool allDigits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) {
    return std::isdigit(c) != 0;
  });
}

Result<uint16_t> parsePort(std::string_view text) {
  unsigned value = 0;
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (!allDigits(text) || ec != std::errc{} || ptr != text.data() + text.size() || value == 0 ||
      value > 65535) {
    return fail(ErrorCode::InvalidArgument, "invalid port '" + std::string(text) + "'");
  }
  return static_cast<uint16_t>(value);
}

Result<void> splitHostPort(std::string_view authority, std::string &host, uint16_t &port) {
  port = 0;
  if (!authority.empty() && authority.front() == '[') {
    auto close = authority.find(']');
    if (close == std::string_view::npos) {
      return fail(ErrorCode::InvalidArgument, "unterminated '[' in host");
    }
    host = std::string(authority.substr(1, close - 1));
    auto rest = authority.substr(close + 1);
    if (rest.empty()) return {};
    if (rest.front() != ':') {
      return fail(ErrorCode::InvalidArgument, "unexpected text after ']' in host");
    }
    auto parsed = parsePort(rest.substr(1));
    if (!parsed) return unexpected<Error>(parsed.error());
    port = *parsed;
    return {};
  }

  auto colon = authority.rfind(':');
  const bool singleColon = colon != std::string_view::npos && authority.find(':') == colon;
  if (singleColon) {
    host = std::string(authority.substr(0, colon));
    auto parsed = parsePort(authority.substr(colon + 1));
    if (!parsed) return unexpected<Error>(parsed.error());
    port = *parsed;
  } else {
    host = std::string(authority);
  }
  return {};
}

Result<void> parseOptions(std::string_view query, Endpoint &endpoint) {
  while (!query.empty()) {
    auto amp = query.find('&');
    auto pair = query.substr(0, amp);
    query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
    if (pair.empty()) continue;

    auto eq = pair.find('=');
    std::string key(pair.substr(0, eq));
    std::string value = eq == std::string_view::npos ? std::string() : std::string(pair.substr(eq + 1));
    if (key.empty()) {
      return fail(ErrorCode::InvalidArgument, "empty option name");
    }
    if (key == "timeout") {
      long long ms = 0;
      auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), ms);
      const bool digits = allDigits(value);
      const bool overflow = digits && ec == std::errc::result_out_of_range;
      if (overflow) {
        ms = Endpoint::kMaxTimeout.count();
      } else if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size() || ms < 0) {
        return fail(ErrorCode::InvalidArgument, "invalid timeout '" + value + "'");
      }
      endpoint.timeout = std::min(std::chrono::milliseconds(ms), Endpoint::kMaxTimeout);
    } else {
      endpoint.options[std::move(key)] = std::move(value);
    }
  }
  return {};
}

} // namespace

Result<Endpoint> Endpoint::parse(std::string_view uri) {
  if (uri.empty()) {
    return fail(ErrorCode::InvalidArgument, "empty endpoint");
  }

  Endpoint endpoint;
  std::string_view rest = uri;

  auto sep = rest.find("://");
  if (sep == std::string_view::npos) {
    endpoint.scheme = "tcp";
  } else {
    auto scheme = rest.substr(0, sep);
    if (!validScheme(scheme)) {
      return fail(ErrorCode::InvalidArgument, "invalid scheme '" + std::string(scheme) + "'");
    }
    endpoint.scheme = toLower(scheme);
    rest = rest.substr(sep + 3);
  }

  std::string_view query;
  auto qmark = rest.find('?');
  if (qmark != std::string_view::npos) {
    query = rest.substr(qmark + 1);
    rest = rest.substr(0, qmark);
  }

  if (auto r = splitHostPort(rest, endpoint.host, endpoint.port); !r) {
    return unexpected<Error>(r.error());
  }
  if (endpoint.host.empty()) {
    return fail(ErrorCode::InvalidArgument, "endpoint '" + std::string(uri) + "' has no host");
  }
  if (auto r = parseOptions(query, endpoint); !r) {
    return unexpected<Error>(r.error());
  }
  return endpoint;
}

std::string Endpoint::toString() const {
  std::string out = scheme;
  out += "://";
  if (host.find(':') != std::string::npos) {
    out += '[' + host + ']';
  } else {
    out += host;
  }
  if (port != 0) {
    out += ':' + std::to_string(port);
  }

  char separator = '?';
  for (const auto &[key, value] : options) {
    out += separator;
    out += key;
    out += '=';
    out += value;
    separator = '&';
  }
  if (timeout != kDefaultTimeout) {
    out += separator;
    out += "timeout=" + std::to_string(timeout.count());
  }
  return out;
}

} // namespace updclient::net
