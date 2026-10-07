#include "cli/output.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <format>
#include <iostream>

namespace updclient::cli {

void Output::writeDocument(const nlohmann::json &document) {
  if (documentWritten_) {
    spdlog::debug("dropping a second JSON document: stdout carries exactly one");
    return;
  }
  documentWritten_ = true;
  std::cout << document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
  std::cout.flush();
}

void Output::result(const nlohmann::json &data, std::string_view text) {
  if (json_) {
    writeDocument(data);
    return;
  }
  if (text.empty()) return;
  std::cout << text;
  if (text.back() != '\n') std::cout << "\n";
  std::cout.flush();
}

void Output::error(std::string_view code, std::string_view message, int sysError, int consoleStatus) {
  if (!json_) return;
  nlohmann::json error = {{"code", std::string(code)}, {"message", std::string(message)}};
  if (sysError != 0) error["os_error"] = sysError;
  if (consoleStatus != 0) error["console_status"] = consoleStatus;
  writeDocument({{"error", std::move(error)}});
}

std::string terminalText(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E) out += std::format("\\x{:02x}", u);
    else out += c;
  }
  return out;
}

std::string hexDump(std::span<const uint8_t> bytes, uint64_t baseOffset) {
  constexpr size_t kPerLine = 16;
  std::string out;
  for (size_t offset = 0; offset < bytes.size(); offset += kPerLine) {
    const size_t count = std::min(kPerLine, bytes.size() - offset);
    out += std::format("{:08X} ", baseOffset + offset);
    for (size_t i = 0; i < kPerLine; ++i) {
      if (i < count) {
        out += std::format(" {:02X}", bytes[offset + i]);
      } else {
        out += "   ";
      }
    }
    out += "  |";
    for (size_t i = 0; i < count; ++i) {
      const uint8_t c = bytes[offset + i];
      out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
    }
    out += "|\n";
  }
  return out;
}

} // namespace updclient::cli
