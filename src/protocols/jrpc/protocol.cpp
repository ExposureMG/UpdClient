#include <protocols/jrpc/protocol.hpp>

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <type_traits>

namespace updclient::jrpc {

std::string_view opcodeName(Opcode opcode) noexcept {
  switch (opcode) {
  case Opcode::ResolveFunction: return "ResolveFunction";
  case Opcode::GetCpuKey: return "GetCpuKey";
  case Opcode::ShutDownConsole: return "ShutDownConsole";
  case Opcode::XNotify: return "XNotify";
  case Opcode::GetKernelVersion: return "GetKernelVersion";
  case Opcode::SetLeds: return "SetLeds";
  case Opcode::GetTemperature: return "GetTemperature";
  case Opcode::GetCurrentTitleId: return "GetCurrentTitleId";
  case Opcode::ConsoleType: return "ConsoleType";
  case Opcode::ConstantMemorySet: return "ConstantMemorySet";
  }
  return "Unknown";
}

std::string_view consoleTypeName(ConsoleType type) noexcept {
  switch (type) {
  case ConsoleType::Xenon: return "Xenon";
  case ConsoleType::Zephyr: return "Zephyr";
  case ConsoleType::Falcon: return "Falcon";
  case ConsoleType::Jasper: return "Jasper";
  case ConsoleType::Trinity: return "Trinity";
  case ConsoleType::Corona: return "Corona";
  case ConsoleType::Unknown: return "Unknown";
  }
  return "Unknown";
}

namespace {

// ---- Requests ---------------------------------------------------------------------

template <class T> void appendDecimal(std::string &out, T value) {
  std::array<char, 24> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  out.append(buffer.data(), result.ptr);
}

std::string upperHex(uint32_t value) {
  std::array<char, 8> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
  std::string out(buffer.data(), result.ptr);
  for (char &c : out) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF. NUL is
// valid UTF-8 and is refused by the caller.
bool isValidUtf8(std::string_view text) noexcept {
  size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<uint8_t>(text[i]);
    if (lead < 0x80) {
      ++i;
      continue;
    }
    size_t extra = 0;
    uint32_t codePoint = 0;
    uint32_t smallest = 0;
    if ((lead & 0xE0) == 0xC0) {
      extra = 1;
      codePoint = lead & 0x1Fu;
      smallest = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
      extra = 2;
      codePoint = lead & 0x0Fu;
      smallest = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
      extra = 3;
      codePoint = lead & 0x07u;
      smallest = 0x10000;
    } else {
      return false;
    }
    if (text.size() - i <= extra) return false;
    for (size_t k = 1; k <= extra; ++k) {
      const auto next = static_cast<uint8_t>(text[i + k]);
      if ((next & 0xC0) != 0x80) return false;
      codePoint = (codePoint << 6) | (next & 0x3Fu);
    }
    if (codePoint < smallest || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
      return false;
    }
    i += extra + 1;
  }
  return true;
}

// The module name goes between quotes in the command line, so nothing that could end
// the quote, split the field or be read as an escape is allowed.
Result<void> checkModuleName(std::string_view name) {
  if (name.empty()) return fail(ErrorCode::InvalidArgument, "a module name must not be empty");
  for (char c : name) {
    const auto u = static_cast<uint8_t>(c);
    if (u <= 0x20 || u >= 0x7F || c == '"' || c == '\\') {
      return fail(ErrorCode::InvalidArgument,
                  "a module name is printable ASCII without spaces, quotes or backslashes");
    }
  }
  return {};
}

Error lineTooLong(size_t limit) {
  return makeError(ErrorCode::LimitExceeded, "a JRPC command line is limited to " + std::to_string(limit) + " bytes");
}

Result<std::string> encodeFloat(double value, int precision) {
  if (!std::isfinite(value)) return fail(ErrorCode::InvalidArgument, "NaN and infinity cannot be sent as a JRPC argument");
  std::array<char, 64> buffer{};
  const auto result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general, precision);
  if (result.ec != std::errc{}) return fail(ErrorCode::InvalidArgument, "a float argument could not be formatted");
  std::string out = "3\\";
  out.append(buffer.data(), result.ptr);
  out += '\\';
  return out;
}

// `<tag>/<byte count>\<hex>\`
std::string encodeBlob(char tag, std::span<const uint8_t> bytes) {
  std::string out;
  out += tag;
  out += '/';
  appendDecimal(out, bytes.size());
  out += '\\';
  out += formatHex(bytes);
  out += '\\';
  return out;
}

void appendBigEndian32(std::vector<uint8_t> &out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

// payloadBytes of a blob argument must fit the line once hex doubled; checked before the
// payload is copied or encoded.
bool payloadFits(size_t payloadBytes, size_t maxCommandBytes) noexcept {
  return payloadBytes <= maxCommandBytes / 2;
}

Result<void> appendArguments(std::string &out, std::span<const Arg> args, size_t maxCommandBytes) {
  for (const Arg &arg : args) {
    auto text = encodeArgument(arg, maxCommandBytes);
    if (!text) return fail(text.error().code, text.error().message);
    out += *text;
    if (out.size() > maxCommandBytes) return unexpected<Error>(lineTooLong(maxCommandBytes));
  }
  return {};
}

// The `params="..."` field and the closing check of the whole line.
Result<void> appendParams(std::string &line, uint32_t address, std::span<const Arg> args, size_t maxCommandBytes) {
  if (args.size() > kMaxArgs) {
    return fail(ErrorCode::LimitExceeded, "a JRPC call takes at most " + std::to_string(kMaxArgs) + " arguments");
  }
  line += " params=\"A\\";
  line += upperHex(address);
  line += "\\A\\";
  appendDecimal(line, args.size());
  line += '\\';
  if (auto added = appendArguments(line, args, maxCommandBytes); !added) return added;
  line += '"';
  if (line.size() > maxCommandBytes) return unexpected<Error>(lineTooLong(maxCommandBytes));
  return {};
}

// ---- Replies ----------------------------------------------------------------------

// At most 48 characters of the line, escaped, for a message.
std::string shown(std::string_view line) {
  constexpr size_t kMaxShown = 48;
  static constexpr char digits[] = "0123456789abcdef";
  std::string out = "\"";
  for (size_t i = 0; i < line.size() && i < kMaxShown; ++i) {
    const auto u = static_cast<uint8_t>(line[i]);
    if (u >= 0x20 && u < 0x7F && line[i] != '"' && line[i] != '\\') {
      out += line[i];
    } else {
      out += "\\x";
      out += digits[u >> 4];
      out += digits[u & 0xF];
    }
  }
  if (line.size() > kMaxShown) out += "...";
  out += '"';
  return out;
}

Error replyError(std::string_view what, std::string_view line) {
  return makeError(ErrorCode::Protocol, std::string(what) + ": " + shown(line));
}

int hexValue(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 1 to maxDigits hex digits and nothing else (maxDigits <= 16).
std::optional<uint64_t> hexNumber(std::string_view text, size_t maxDigits) noexcept {
  if (text.empty() || text.size() > maxDigits) return std::nullopt;
  uint64_t value = 0;
  for (char c : text) {
    const int digit = hexValue(c);
    if (digit < 0) return std::nullopt;
    value = (value << 4) | static_cast<uint64_t>(digit);
  }
  return value;
}

std::optional<int32_t> decimalInt32(std::string_view text) noexcept {
  int32_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

std::optional<double> decimalDouble(std::string_view text) noexcept {
  double value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

// `v,v,...,v;` -> the values, each read by parse(token) -> optional<T>.
template <class T, class Parse>
Result<std::vector<T>> parseList(std::string_view line, size_t maxElements, std::string_view what, Parse parse) {
  if (line.empty() || line.back() != ';') return unexpected<Error>(replyError(std::string(what) + " must end with ';'", line));
  std::string_view rest = line.substr(0, line.size() - 1);
  if (rest.empty()) return unexpected<Error>(replyError(std::string(what) + " has no elements", line));
  std::vector<T> values;
  while (true) {
    const size_t comma = rest.find(',');
    const std::string_view token = rest.substr(0, comma);
    const auto value = parse(token);
    if (!value) return unexpected<Error>(replyError(std::string(what) + " has a bad element", line));
    if (values.size() >= maxElements) {
      return unexpected<Error>(replyError(std::string(what) + " has more than " + std::to_string(maxElements) + " elements", line));
    }
    values.push_back(*value);
    if (comma == std::string_view::npos) break;
    rest.remove_prefix(comma + 1);
  }
  return values;
}

} // namespace

Result<std::string> encodeArgument(const Arg &arg, size_t maxCommandBytes) {
  struct Visitor {
    size_t limit;

    Result<std::string> operator()(int32_t v) const {
      std::string out = "1\\";
      appendDecimal(out, v);
      out += '\\';
      return out;
    }
    Result<std::string> operator()(uint32_t v) const {
      // The server reads tag 1 with sscanf %i into an int: past INT32_MAX the equal
      // negative number is the portable spelling.
      return (*this)(static_cast<int32_t>(v));
    }
    Result<std::string> operator()(bool v) const { return std::string(v ? "1/1\\" : "1/0\\"); }
    Result<std::string> operator()(uint8_t v) const {
      std::string out = "4\\";
      appendDecimal(out, static_cast<unsigned>(v));
      out += '\\';
      return out;
    }
    Result<std::string> operator()(int64_t v) const {
      std::string out = "8\\";
      appendDecimal(out, v);
      out += '\\';
      return out;
    }
    Result<std::string> operator()(uint64_t v) const { return (*this)(static_cast<int64_t>(v)); }
    Result<std::string> operator()(float v) const { return encodeFloat(static_cast<double>(v), 9); }
    Result<std::string> operator()(double v) const { return encodeFloat(v, 17); }
    Result<std::string> operator()(const std::string &v) const {
      if (!payloadFits(v.size(), limit)) return unexpected<Error>(lineTooLong(limit));
      if (v.find('\0') != std::string::npos) {
        return fail(ErrorCode::InvalidArgument, "a string argument must not contain a NUL byte");
      }
      if (!isValidUtf8(v)) return fail(ErrorCode::InvalidArgument, "a string argument must be valid UTF-8");
      return encodeBlob('2', std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(v.data()), v.size()));
    }
    Result<std::string> operator()(const std::vector<uint8_t> &v) const {
      if (!payloadFits(v.size(), limit)) return unexpected<Error>(lineTooLong(limit));
      return encodeBlob('7', v);
    }
    Result<std::string> operator()(const std::vector<int32_t> &v) const {
      if (v.size() > (std::numeric_limits<size_t>::max)() / 4 || !payloadFits(v.size() * 4, limit)) {
        return unexpected<Error>(lineTooLong(limit));
      }
      std::vector<uint8_t> bytes;
      bytes.reserve(v.size() * 4);
      for (int32_t element : v) appendBigEndian32(bytes, static_cast<uint32_t>(element));
      return encodeBlob('7', bytes);
    }
    Result<std::string> operator()(const std::vector<float> &v) const {
      if (v.size() > (std::numeric_limits<size_t>::max)() / 4 || !payloadFits(v.size() * 4, limit)) {
        return unexpected<Error>(lineTooLong(limit));
      }
      std::vector<uint8_t> bytes;
      bytes.reserve(v.size() * 4);
      for (float element : v) appendBigEndian32(bytes, std::bit_cast<uint32_t>(element));
      return encodeBlob('7', bytes);
    }
  };
  return std::visit(Visitor{maxCommandBytes}, arg.value);
}

Result<std::string> buildCommand(const CallSpec &spec, size_t maxCommandBytes, size_t maxArrayElements) {
  const auto tag = returnTag(spec.returns);
  if (!tag) {
    return fail(ErrorCode::Unsupported,
                "a 64-bit array return cannot be requested: type 9 is ResolveFunction on the JRPC server");
  }
  if (isArrayKind(spec.returns)) {
    if (spec.arraySize == 0) return fail(ErrorCode::InvalidArgument, "an array return needs at least one element");
    if (spec.arraySize > maxArrayElements) {
      return fail(ErrorCode::LimitExceeded,
                  "an array return is limited to " + std::to_string(maxArrayElements) + " elements");
    }
  } else if (spec.arraySize != 0) {
    return fail(ErrorCode::InvalidArgument, "arraySize applies only to the array return kinds");
  }

  uint32_t address = 0;
  const ByName *named = std::get_if<ByName>(&spec.target);
  if (named) {
    if (auto ok = checkModuleName(named->module); !ok) return fail(ok.error().code, ok.error().message);
  } else {
    address = std::get<uint32_t>(spec.target);
    if (address == 0) {
      return fail(ErrorCode::InvalidArgument, "a call at address 0 is refused: 0 marks a call by ordinal");
    }
  }

  std::string line = "consolefeatures ver=2 type=";
  appendDecimal(line, static_cast<unsigned>(*tag));
  if (spec.thread == ThreadContext::System) line += " system";
  if (named) {
    line += " module=\"";
    line += named->module;
    line += "\" ord=";
    appendDecimal(line, named->ordinal);
  }
  line += " as=";
  appendDecimal(line, spec.arraySize);
  if (auto params = appendParams(line, address, spec.args, maxCommandBytes); !params) {
    return fail(params.error().code, params.error().message);
  }
  return line;
}

Result<std::string> buildOpcodeCommand(Opcode opcode, std::span<const Arg> args, uint32_t address,
                                       size_t maxCommandBytes) {
  if (!isKnownOpcode(opcode)) {
    return fail(ErrorCode::InvalidArgument, "unknown JRPC opcode " + std::to_string(static_cast<unsigned>(opcode)));
  }
  std::string line = "consolefeatures ver=2 type=";
  appendDecimal(line, static_cast<unsigned>(opcode));
  if (auto params = appendParams(line, address, args, maxCommandBytes); !params) {
    return fail(params.error().code, params.error().message);
  }
  return line;
}

std::string formatLine(std::string_view line) {
  std::string out;
  out.reserve(line.size() + 2);
  out.append(line);
  out += "\r\n";
  return out;
}

std::string_view stripTerminator(std::string_view line) noexcept {
  if (line.ends_with("\r\n")) return line.substr(0, line.size() - 2);
  if (line.ends_with('\n')) return line.substr(0, line.size() - 1);
  return line;
}

bool isErrorLine(std::string_view line) noexcept {
  return line.starts_with("error=");
}

std::string_view errorText(std::string_view line) noexcept {
  return isErrorLine(line) ? line.substr(6) : std::string_view();
}

bool isDebugLine(std::string_view line) noexcept {
  return line.find("DEBUG") != std::string_view::npos;
}

Result<void> parseVoidReply(std::string_view line) {
  if (line == "S_OK" || hexNumber(line, 16)) return {};
  return unexpected<Error>(replyError("a void call answers S_OK or hex", line));
}

Result<uint32_t> parseIntReply(std::string_view line) {
  const auto value = hexNumber(line, 8);
  if (!value) return unexpected<Error>(replyError("expected up to 8 hex digits", line));
  return static_cast<uint32_t>(*value);
}

Result<uint8_t> parseByteReply(std::string_view line) {
  const auto value = hexNumber(line, 8);
  if (!value) return unexpected<Error>(replyError("expected a hex byte", line));
  return static_cast<uint8_t>(*value & 0xFF);
}

Result<uint64_t> parseInt64Reply(std::string_view line) {
  const auto value = hexNumber(line, 16);
  if (!value) return unexpected<Error>(replyError("expected up to 16 hex digits", line));
  return *value;
}

Result<double> parseFloatReply(std::string_view line) {
  const auto value = decimalDouble(line);
  if (!value) return unexpected<Error>(replyError("expected a decimal number", line));
  return *value;
}

Result<std::string> parseStringReply(std::string_view line) {
  for (char c : line) {
    if (c == '\0' || c == '\r' || c == '\n') return unexpected<Error>(replyError("a string reply holds a control character", line));
  }
  return std::string(line);
}

Result<int32_t> parseDecimalReply(std::string_view line) {
  const auto value = decimalInt32(line);
  if (!value) return unexpected<Error>(replyError("expected a decimal number", line));
  return *value;
}

Result<std::vector<int32_t>> parseIntArrayReply(std::string_view line, size_t maxElements) {
  return parseList<int32_t>(line, maxElements, "an int array", [](std::string_view token) { return decimalInt32(token); });
}

Result<std::vector<double>> parseFloatArrayReply(std::string_view line, size_t maxElements) {
  return parseList<double>(line, maxElements, "a float array", [](std::string_view token) { return decimalDouble(token); });
}

Result<std::vector<uint8_t>> parseByteArrayReply(std::string_view line, size_t maxElements) {
  return parseList<uint8_t>(line, maxElements, "a byte array", [](std::string_view token) -> std::optional<uint8_t> {
    const auto value = hexNumber(token, 8);
    if (!value) return std::nullopt;
    return static_cast<uint8_t>(*value & 0xFF);
  });
}

Result<CallValue> parseReply(ReturnKind kind, std::string_view line, size_t arraySize) {
  const auto wrap = [](auto &&result) -> Result<CallValue> {
    if (!result) return unexpected<Error>(result.error());
    return CallValue(std::move(*result));
  };
  const auto sized = [&](auto &&result) -> Result<CallValue> {
    if (!result) return unexpected<Error>(result.error());
    if (result->size() != arraySize) {
      return unexpected<Error>(replyError("an array reply holds " + std::to_string(result->size()) +
                                              " elements, " + std::to_string(arraySize) + " were requested",
                                          line));
    }
    return CallValue(std::move(*result));
  };
  switch (kind) {
  case ReturnKind::Void: {
    auto result = parseVoidReply(line);
    if (!result) return unexpected<Error>(result.error());
    return CallValue(std::monostate{});
  }
  case ReturnKind::Int: {
    auto result = parseIntReply(line);
    if (!result) return unexpected<Error>(result.error());
    return CallValue(static_cast<uint64_t>(*result));
  }
  case ReturnKind::Byte: {
    auto result = parseByteReply(line);
    if (!result) return unexpected<Error>(result.error());
    return CallValue(static_cast<uint64_t>(*result));
  }
  case ReturnKind::Int64: return wrap(parseInt64Reply(line));
  case ReturnKind::String: return wrap(parseStringReply(line));
  case ReturnKind::Float: return wrap(parseFloatReply(line));
  case ReturnKind::IntArray:
    if (arraySize == 0) return fail(ErrorCode::InvalidArgument, "an array reply needs a requested size");
    return sized(parseIntArrayReply(line, arraySize));
  case ReturnKind::FloatArray:
    if (arraySize == 0) return fail(ErrorCode::InvalidArgument, "an array reply needs a requested size");
    return sized(parseFloatArrayReply(line, arraySize));
  case ReturnKind::ByteArray:
    if (arraySize == 0) return fail(ErrorCode::InvalidArgument, "an array reply needs a requested size");
    return sized(parseByteArrayReply(line, arraySize));
  case ReturnKind::Uint64Array:
    return fail(ErrorCode::Unsupported, "a 64-bit array return is not available over JRPC");
  }
  return fail(ErrorCode::InvalidArgument, "unknown return kind");
}

Result<CpuKey> parseCpuKey(std::string_view line) {
  if (line.size() != 16 && line.size() != 32) {
    return unexpected<Error>(replyError(
        "the CPU key reply is not 16 or 32 hex digits (the server prints two unpadded halves)", line));
  }
  CpuKey key;
  key.bytes.resize(line.size() / 2);
  if (auto parsed = parseHex(line, key.bytes); !parsed) {
    return unexpected<Error>(replyError("the CPU key reply is not hex", line));
  }
  return key;
}

Result<ConsoleType> parseConsoleType(std::string_view line) {
  for (ConsoleType type : {ConsoleType::Xenon, ConsoleType::Zephyr, ConsoleType::Falcon, ConsoleType::Jasper,
                           ConsoleType::Trinity, ConsoleType::Corona, ConsoleType::Unknown}) {
    if (line == consoleTypeName(type)) return type;
  }
  return unexpected<Error>(replyError("not a console type name", line));
}

} // namespace updclient::jrpc
