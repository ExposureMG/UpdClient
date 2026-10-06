#include <updclient/core/hex.hpp>

namespace updclient {

namespace {

int hexValue(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

} // namespace

std::string formatHex(std::span<const uint8_t> bytes) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t b : bytes) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0x0F]);
  }
  return out;
}

Result<void> parseHex(std::string_view hex, std::span<uint8_t> out) {
  if (hex.size() != out.size() * 2) {
    return fail(ErrorCode::InvalidArgument,
                "expected " + std::to_string(out.size() * 2) + " hex digits, got " +
                    std::to_string(hex.size()));
  }
  for (size_t i = 0; i < out.size(); ++i) {
    int hi = hexValue(hex[i * 2]);
    int lo = hexValue(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return fail(ErrorCode::InvalidArgument, "invalid hex digit near offset " + std::to_string(i * 2));
    }
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return {};
}

} // namespace updclient
