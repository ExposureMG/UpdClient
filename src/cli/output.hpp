#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace updclient::cli {

// Everything written to stdout goes through here. In JSON mode stdout receives
// exactly one document: the first result or error; anything later is dropped.
class Output {
public:
  void setJson(bool enabled) noexcept { json_ = enabled; }
  bool json() const noexcept { return json_; }

  // Text mode prints text (a trailing newline is added when missing, nothing is
  // printed for empty text); JSON mode prints data.
  void result(const nlohmann::json &data, std::string_view text);
  // JSON mode only; the human readable message is logged to stderr by the caller.
  void error(std::string_view code, std::string_view message, int sysError = 0, int consoleStatus = 0);

private:
  void writeDocument(const nlohmann::json &document);

  bool json_ = false;
  bool documentWritten_ = false;
};

// 16 bytes per line: offset, hex bytes, printable ASCII.
std::string hexDump(std::span<const uint8_t> bytes, uint64_t baseOffset = 0);

} // namespace updclient::cli
