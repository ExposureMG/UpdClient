#include <protocols/xbdm/protocol.hpp>

#include <cctype>

namespace updclient::xbdm {

// nullopt for the few FILETIMEs past the end of the tick range.
std::optional<FileTimePoint> fileTimeToTimePoint(uint64_t fileTime) noexcept {
  constexpr uint64_t kMaxTicks = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
  if (fileTime >= kFileTimeUnixEpoch) {
    const uint64_t ticks = fileTime - kFileTimeUnixEpoch;
    if (ticks > kMaxTicks) return std::nullopt;
    return FileTimePoint(FileTimeTicks(static_cast<int64_t>(ticks)));
  }
  return FileTimePoint(FileTimeTicks(-static_cast<int64_t>(kFileTimeUnixEpoch - fileTime)));
}

// nullopt before 1601-01-01.
std::optional<uint64_t> timePointToFileTime(FileTimePoint time) noexcept {
  const int64_t ticks = time.time_since_epoch().count();
  if (ticks >= 0) return kFileTimeUnixEpoch + static_cast<uint64_t>(ticks);
  const uint64_t before = uint64_t{0} - static_cast<uint64_t>(ticks);
  if (before > kFileTimeUnixEpoch) return std::nullopt;
  return kFileTimeUnixEpoch - before;
}

namespace {

char lower(char c) noexcept {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string lowered(std::string_view text) {
  std::string out(text);
  for (char &c : out) c = lower(c);
  return out;
}

bool isDigit(char c) noexcept {
  return c >= '0' && c <= '9';
}

int hexValue(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool isSeparator(char c) noexcept {
  return c == ' ' || c == '\t';
}

bool isPrintableAscii(uint8_t c) noexcept {
  return c >= 0x20 && c <= 0x7E;
}

} // namespace

std::optional<StatusLine> parseStatusLine(std::string_view line) {
  if (line.size() < 4 || !isDigit(line[0]) || !isDigit(line[1]) || !isDigit(line[2]) || line[3] != '-') {
    return std::nullopt;
  }
  StatusLine status;
  status.code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
  std::string_view text = line.substr(4);
  if (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  status.text = std::string(text);
  return status;
}

Params parseParams(std::string_view line) {
  Params params;
  size_t pos = 0;
  while (pos < line.size()) {
    if (isSeparator(line[pos])) {
      ++pos;
      continue;
    }
    const size_t keyStart = pos;
    while (pos < line.size() && !isSeparator(line[pos]) && line[pos] != '=') ++pos;
    const std::string_view key = line.substr(keyStart, pos - keyStart);

    if (pos >= line.size() || line[pos] != '=') {
      std::string_view flag = key;
      if (flag.size() > 1 && flag.back() == ',') flag.remove_suffix(1);
      params.flags.push_back(lowered(flag));
      continue;
    }
    ++pos;

    std::string value;
    if (pos < line.size() && line[pos] == '"') {
      const size_t close = line.find('"', pos + 1);
      if (close == std::string_view::npos) {
        params.malformed = true;
        value = std::string(line.substr(pos + 1));
        pos = line.size();
      } else {
        value = std::string(line.substr(pos + 1, close - pos - 1));
        pos = close + 1;
        if (pos < line.size() && !isSeparator(line[pos])) {
          params.malformed = true;
          while (pos < line.size() && !isSeparator(line[pos])) ++pos;
        }
      }
    } else {
      const size_t valueStart = pos;
      while (pos < line.size() && !isSeparator(line[pos])) ++pos;
      std::string_view raw = line.substr(valueStart, pos - valueStart);
      if (!raw.empty() && raw.back() == ',') raw.remove_suffix(1);
      value = std::string(raw);
    }

    if (key.empty()) {
      params.malformed = true;
      continue;
    }
    params.values.emplace_back(lowered(key), std::move(value));
  }
  return params;
}

std::optional<uint64_t> parseNumber64(std::string_view text) noexcept {
  if (text.empty()) return std::nullopt;
  uint64_t value = 0;
  if (text.size() >= 2 && text[0] == '0' && (lower(text[1]) == 'x' || lower(text[1]) == 'q')) {
    text.remove_prefix(2);
    if (text.empty()) return std::nullopt;
    for (char c : text) {
      const int digit = hexValue(c);
      if (digit < 0) return std::nullopt;
      if (value > ((std::numeric_limits<uint64_t>::max)() >> 4)) return std::nullopt;
      value = (value << 4) | static_cast<uint64_t>(digit);
    }
    return value;
  }
  for (char c : text) {
    if (!isDigit(c)) return std::nullopt;
    const auto digit = static_cast<uint64_t>(c - '0');
    if (value > ((std::numeric_limits<uint64_t>::max)() - digit) / 10) return std::nullopt;
    value = value * 10 + digit;
  }
  return value;
}

std::optional<uint32_t> parseNumber32(std::string_view text) noexcept {
  const auto value = parseNumber64(text);
  if (!value || *value > (std::numeric_limits<uint32_t>::max)()) return std::nullopt;
  return static_cast<uint32_t>(*value);
}

std::string formatNumber(uint64_t value) {
  static constexpr char digits[] = "0123456789abcdef";
  char buffer[16];
  unsigned count = 0;
  do {
    buffer[count++] = digits[value & 0xF];
    value >>= 4;
  } while (value != 0);
  std::string out = "0x";
  while (count > 0) out.push_back(buffer[--count]);
  return out;
}

std::vector<uint8_t> makeWildcardQuery() {
  return {kNameWildcard, 0};
}

Result<std::vector<uint8_t>> makeNameLookup(std::string_view name) {
  if (name.empty() || name.size() > 255) {
    return fail(ErrorCode::InvalidArgument, "a console name has 1 to 255 characters");
  }
  for (char c : name) {
    if (!isPrintableAscii(static_cast<uint8_t>(c))) {
      return fail(ErrorCode::InvalidArgument, "a console name is printable ASCII");
    }
  }
  std::vector<uint8_t> packet;
  packet.reserve(name.size() + 2);
  packet.push_back(kNameLookup);
  packet.push_back(static_cast<uint8_t>(name.size()));
  packet.insert(packet.end(), name.begin(), name.end());
  return packet;
}

std::optional<std::string> parseNameReply(std::span<const uint8_t> datagram) {
  if (datagram.size() < 2 || datagram[0] != kNameReply) return std::nullopt;
  const size_t length = datagram[1];
  if (length == 0 || length > datagram.size() - 2) return std::nullopt;
  std::string name;
  name.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = datagram[2 + i];
    if (!isPrintableAscii(c)) return std::nullopt;
    name.push_back(static_cast<char>(c));
  }
  return name;
}

} // namespace updclient::xbdm
