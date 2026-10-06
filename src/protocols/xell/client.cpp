#include <updclient/protocols/xell/client.hpp>

#include <updclient/core/hex.hpp>
#include <updclient/core/path.hpp>
#include <updclient/net/transport_registry.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>

namespace updclient::xell {

namespace {

constexpr size_t kMaxPageBytes = 1024 * 1024;
constexpr size_t kMaxKeyvaultBytes = 16 * 1024 * 1024;
constexpr size_t kKeyHexDigits = 32;
constexpr size_t kColorHexDigits = 6;
constexpr size_t kLabelWindow = 96;

bool isHexDigit(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isWordChar(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool atBoundaryBefore(std::string_view text, size_t pos) noexcept {
  return pos == 0 || !isWordChar(text[pos - 1]);
}

bool atBoundaryAfter(std::string_view text, size_t pos) noexcept {
  return pos >= text.size() || !isWordChar(text[pos]);
}

enum class Label { None, Cpu, Dvd };

struct KeyCandidate {
  size_t begin = 0;
  size_t end = 0;
  Label label = Label::None;
};

// The nearest of "cpu" / "dvd" within the text just before a key.
Label labelBefore(std::string_view text, size_t begin, size_t floor) {
  const size_t start = std::max(floor, begin > kLabelWindow ? begin - kLabelWindow : size_t{0});
  std::string window(text.substr(start, begin - start));
  for (char &c : window) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  const auto cpu = window.rfind("cpu");
  const auto dvd = window.rfind("dvd");
  if (cpu == std::string::npos && dvd == std::string::npos) return Label::None;
  if (dvd == std::string::npos) return Label::Cpu;
  if (cpu == std::string::npos) return Label::Dvd;
  return cpu > dvd ? Label::Cpu : Label::Dvd;
}

std::vector<KeyCandidate> findKeyCandidates(std::string_view text) {
  std::vector<KeyCandidate> found;
  size_t floor = 0;
  size_t i = 0;
  while (i < text.size()) {
    if (!isHexDigit(text[i])) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < text.size() && isHexDigit(text[j])) ++j;
    if (j - i == kKeyHexDigits && atBoundaryBefore(text, i) && atBoundaryAfter(text, j)) {
      found.push_back({i, j, labelBefore(text, i, floor)});
      floor = j;
    }
    i = j;
  }
  return found;
}

bool decodeKey(std::string_view text, const KeyCandidate &candidate, std::array<uint8_t, 16> &out) {
  return static_cast<bool>(parseHex(text.substr(candidate.begin, candidate.end - candidate.begin), out));
}

std::vector<std::string> findColors(std::string_view text) {
  std::vector<std::string> colors;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '#') continue;
    // "&#123456;" is a character reference, not a colour.
    if (i > 0 && (isWordChar(text[i - 1]) || text[i - 1] == '&')) continue;
    const size_t digits = i + 1;
    const size_t end = digits + kColorHexDigits;
    if (end > text.size()) break;
    bool allHex = true;
    for (size_t k = digits; k < end; ++k) {
      if (!isHexDigit(text[k])) {
        allHex = false;
        break;
      }
    }
    if (!allHex || !atBoundaryAfter(text, end)) continue;
    colors.emplace_back(text.substr(i, end - i));
    i = end - 1;
  }
  return colors;
}

} // namespace

XellInfo parseXellInfo(std::string_view html) {
  XellInfo info;

  const auto candidates = findKeyCandidates(html);
  std::vector<bool> used(candidates.size(), false);

  // Labelled keys claim their slot first, so page order only matters for the rest.
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (candidates[i].label == Label::Cpu && !info.cpuKeyValid) {
      info.cpuKeyValid = decodeKey(html, candidates[i], info.cpuKey);
      used[i] = true;
    } else if (candidates[i].label == Label::Dvd && !info.dvdKeyValid) {
      info.dvdKeyValid = decodeKey(html, candidates[i], info.dvdKey);
      used[i] = true;
    } else if (candidates[i].label != Label::None) {
      used[i] = true;
    }
  }
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (used[i]) continue;
    if (!info.cpuKeyValid) {
      info.cpuKeyValid = decodeKey(html, candidates[i], info.cpuKey);
    } else if (!info.dvdKeyValid) {
      info.dvdKeyValid = decodeKey(html, candidates[i], info.dvdKey);
    }
  }

  const auto colors = findColors(html);
  if (colors.size() >= 1) info.bgColor = colors[0];
  if (colors.size() >= 2) info.fgColor = colors[1];
  return info;
}

XellClient::XellClient(Connector connector, std::string hostHeader)
    : connector_(std::move(connector)), hostHeader_(std::move(hostHeader)) {}

XellClient XellClient::forEndpoint(const net::Endpoint &endpoint) {
  const net::Endpoint target = net::TransportRegistry::instance().withDefaultPort(endpoint, kXellHttpPort);

  std::string host = target.host.find(':') == std::string::npos ? target.host : "[" + target.host + "]";
  if (target.port != 0 && target.port != kXellHttpPort) host += ":" + std::to_string(target.port);

  return XellClient([target]() { return net::TransportRegistry::instance().connect(target); },
                    std::move(host));
}

Result<net::TransportPtr> XellClient::open() const {
  if (!connector_) return fail(ErrorCode::NotConnected, "XeLL client has no connector");
  auto transport = connector_();
  if (!transport) return transport;
  if (!*transport) return fail(ErrorCode::ConnectFailed, "connector returned no transport");
  return transport;
}

Result<net::HttpResponse> XellClient::fetch(const std::string &path, size_t maxBodyBytes) const {
  auto transport = open();
  if (!transport) return unexpected<Error>(transport.error());

  net::HttpGetOptions options;
  options.maxBodyBytes = maxBodyBytes;
  return net::httpGet(**transport, net::HttpRequest(hostHeader_, path), options);
}

Result<XellInfo> XellClient::getInfo() const {
  auto response = fetch("/", kMaxPageBytes);
  if (!response) return unexpected<Error>(response.error());

  const std::string_view html(reinterpret_cast<const char *>(response->body.data()), response->body.size());
  XellInfo info = parseXellInfo(html);

  const auto missing = info.missingFields();
  if (missing.size() == 4) {
    return fail(ErrorCode::Protocol, "XeLL page holds no CPU key, DVD key or colours (" +
                                         std::to_string(response->body.size()) + " bytes received)");
  }
  if (!missing.empty()) {
    std::string names;
    for (const auto &name : missing) names += (names.empty() ? "" : ", ") + name;
    spdlog::debug("XeLL info page is missing: {}", names);
  }
  return info;
}

Result<void> XellClient::dumpFlash(const std::filesystem::path &outputPath, Progress progress) const {
  auto transport = open();
  if (!transport) return unexpected<Error>(transport.error());

  auto result = net::httpGetToFile(**transport, net::HttpRequest(hostHeader_, "/FLASH"), outputPath,
                                   std::move(progress));
  if (!result) return unexpected<Error>(result.error());
  spdlog::debug("XeLL flash dump: {} bytes written to {}", result->bytes, pathToUtf8(outputPath));
  return {};
}

Result<std::string> XellClient::getFuses() const {
  auto response = fetch("/FUSE", kMaxPageBytes);
  if (!response) return unexpected<Error>(response.error());
  return std::string(response->body.begin(), response->body.end());
}

Result<std::vector<uint8_t>> XellClient::getKeyvault(KeyvaultMode mode) const {
  const char *path = nullptr;
  switch (mode) {
  case KeyvaultMode::Decrypted: path = "/KV"; break;
  case KeyvaultMode::Raw: path = "/KVRAW"; break;
  case KeyvaultMode::RawBlock: path = "/KVRAW2"; break;
  }
  if (!path) return fail(ErrorCode::InvalidArgument, "unknown keyvault mode");

  auto response = fetch(path, kMaxKeyvaultBytes);
  if (!response) return unexpected<Error>(response.error());
  return std::move(response->body);
}

} // namespace updclient::xell
