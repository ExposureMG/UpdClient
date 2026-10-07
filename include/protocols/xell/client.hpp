#pragma once

#include <core/error.hpp>
#include <core/export.hpp>
#include <net/endpoint.hpp>
#include <net/http_lite.hpp>
#include <net/transport.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace updclient::xell {

inline constexpr uint16_t kXellHttpPort = 80;

struct XellInfo {
  std::array<uint8_t, 16> cpuKey{};
  std::array<uint8_t, 16> dvdKey{};
  std::string bgColor; // "#RRGGBB"; empty when not found
  std::string fgColor;
  bool cpuKeyValid = false;
  bool dvdKeyValid = false;

  // Names of the fields that were not found on the page: "cpuKey", "dvdKey",
  // "bgColor", "fgColor".
  std::vector<std::string> missingFields() const {
    std::vector<std::string> missing;
    if (!cpuKeyValid) missing.emplace_back("cpuKey");
    if (!dvdKeyValid) missing.emplace_back("dvdKey");
    if (bgColor.empty()) missing.emplace_back("bgColor");
    if (fgColor.empty()) missing.emplace_back("fgColor");
    return missing;
  }
};

enum class KeyvaultMode {
  Decrypted, // /KV
  Raw,       // /KVRAW
  RawBlock   // /KVRAW2
};

// Extracts the CPU key, DVD key and the two colours from the XeLL index page.
// A key is a run of exactly 32 hex digits bounded by non-word characters; a
// nearby "cpu" or "dvd" label picks the slot, otherwise the first unlabelled
// key is the CPU key and the next the DVD key. A colour is "#" plus exactly six
// hex digits (HTML character references such as &#123456; are ignored); the
// first is the background and the second the foreground.
UPDCLIENT_API XellInfo parseXellInfo(std::string_view html);

// XeLL answers every request with "Connection: close", so each request opens a
// fresh transport through the connector and the client itself stays reusable.
class UPDCLIENT_API XellClient {
public:
  using Connector = std::function<Result<net::TransportPtr>()>;
  using Progress = std::function<void(size_t bytesRead, size_t totalSize)>;

  explicit XellClient(Connector connector, std::string hostHeader = "xell");

  // Connects lazily through the TransportRegistry for every request; port 0
  // selects 80. A missing or unregistered scheme is reported by the request.
  static XellClient forEndpoint(const net::Endpoint &endpoint);

  // Fails with Protocol when the page holds neither keys nor colours.
  Result<XellInfo> getInfo() const;

  // Downloads /FLASH to a temporary file and renames it over outputPath once
  // complete. progress receives (bytesRead, totalSize) with totalSize 0 when XeLL
  // sends no Content-Length; then only the connection closing ends the dump.
  Result<void> dumpFlash(const std::filesystem::path &outputPath, Progress progress = nullptr) const;

  Result<std::string> getFuses() const;
  Result<std::vector<uint8_t>> getKeyvault(KeyvaultMode mode = KeyvaultMode::Decrypted) const;

private:
  Result<net::TransportPtr> open() const;
  Result<net::HttpResponse> fetch(const std::string &path, size_t maxBodyBytes) const;

  Connector connector_;
  std::string hostHeader_;
};

} // namespace updclient::xell
