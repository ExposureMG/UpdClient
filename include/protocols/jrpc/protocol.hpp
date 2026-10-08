#pragma once

#include <core/error.hpp>
#include <core/export.hpp>
#include <core/hex.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Wire-level definitions for JRPC, the TCP plugin JRPC.xex on port 1409: the command
// line builders, the argument encoding and the reply parsers. No I/O lives here, and
// nothing in this file knows about the banner exchange or the connection (the banner
// and `Bye` strings are constants only), so the request side can move to a shared
// directory when a JRPC2 adapter wants the same command text. The protocol itself is
// described in docs/JRPC_PROTOCOL.md and the design in docs/JRPC_ADAPTER_PLAN.md;
// section numbers below refer to the former, D1 to D10 to the latter.
//
// Nothing here has been checked against a console. Every choice the document leaves
// open is marked "Contract:" and is a decision of this library.

namespace updclient::jrpc {

inline constexpr uint16_t kJrpcPort = 1409;

// Section 1.2. The banner is not a version; `Bye` is the polite end of a connection.
inline constexpr std::string_view kBanner = "JRPC2 connected";
inline constexpr std::string_view kBye = "Bye";

// Default bounds. Every one can be changed per client through ClientOptions (or the
// parameters of the builders below), except kMaxArgs, which is the server's frame.
inline constexpr size_t kMaxCommandBytes = 8191;  // one command line without its terminator (section 1.3)
inline constexpr size_t kMaxArgs = 37;            // section 2
inline constexpr size_t kMaxArrayElements = 8;    // section 4.1: the array formats have 8 slots

// type= values 9 to 18 (section 4.2). Opcode 19 is not exposed (open question 4).
enum class Opcode : uint8_t {
  ResolveFunction = 9,
  GetCpuKey = 10,
  ShutDownConsole = 11,
  XNotify = 12,
  GetKernelVersion = 13,
  SetLeds = 14,
  GetTemperature = 15,
  GetCurrentTitleId = 16,
  ConsoleType = 17,
  ConstantMemorySet = 18,
};

// type= values 0 to 8 (section 4.1): how the server formats the return of a call.
enum class ReturnTag : uint8_t {
  Void = 0,
  Int = 1,
  String = 2,
  Float = 3,
  Byte = 4,
  IntArray = 5,
  FloatArray = 6,
  ByteArray = 7,
  Int64 = 8,
};

constexpr bool isKnownOpcode(Opcode opcode) noexcept {
  const auto value = static_cast<uint8_t>(opcode);
  return value >= 9 && value <= 18;
}

// The opcodes that only read (9, 10, 13, 15, 16, 17). Every other opcode, and every
// call, may change the console (D7).
constexpr bool isReadOnly(Opcode opcode) noexcept {
  switch (opcode) {
  case Opcode::ResolveFunction:
  case Opcode::GetCpuKey:
  case Opcode::GetKernelVersion:
  case Opcode::GetTemperature:
  case Opcode::GetCurrentTitleId:
  case Opcode::ConsoleType:
    return true;
  default:
    return false;
  }
}

// The opcodes the server may not answer at all (11, 12, 14, 18; D6).
constexpr bool mayBeSilent(Opcode opcode) noexcept {
  switch (opcode) {
  case Opcode::ShutDownConsole:
  case Opcode::XNotify:
  case Opcode::SetLeds:
  case Opcode::ConstantMemorySet:
    return true;
  default:
    return false;
  }
}

UPDCLIENT_API std::string_view opcodeName(Opcode opcode) noexcept;

// What the caller wants back from a call. Uint64Array exists only to be refused: on
// the TCP server type=9 is ResolveFunction, so a 64-bit array return is unreachable
// (open question 2). buildCommand answers Unsupported for it.
enum class ReturnKind { Void, Int, String, Float, Byte, IntArray, FloatArray, ByteArray, Int64, Uint64Array };

// The type= tag of a kind; nullopt for Uint64Array.
constexpr std::optional<ReturnTag> returnTag(ReturnKind kind) noexcept {
  switch (kind) {
  case ReturnKind::Void: return ReturnTag::Void;
  case ReturnKind::Int: return ReturnTag::Int;
  case ReturnKind::String: return ReturnTag::String;
  case ReturnKind::Float: return ReturnTag::Float;
  case ReturnKind::Byte: return ReturnTag::Byte;
  case ReturnKind::IntArray: return ReturnTag::IntArray;
  case ReturnKind::FloatArray: return ReturnTag::FloatArray;
  case ReturnKind::ByteArray: return ReturnTag::ByteArray;
  case ReturnKind::Int64: return ReturnTag::Int64;
  case ReturnKind::Uint64Array: return std::nullopt;
  }
  return std::nullopt;
}

constexpr bool isArrayKind(ReturnKind kind) noexcept {
  return kind == ReturnKind::IntArray || kind == ReturnKind::FloatArray || kind == ReturnKind::ByteArray ||
         kind == ReturnKind::Uint64Array;
}

enum class ThreadContext { Title, System };

// A function named by an export ordinal of a loaded module instead of an address
// (section 5). The server resolves it; the address field of the command is 0.
struct ByName {
  std::string module;
  uint32_t ordinal = 0;
};

// One argument of a call. A closed set, so nothing is guessed from C++ types at run
// time; use the factories. The encoding (D5, every rule a Contract:) is in
// encodeArgument. Integers and 64-bit values are sent as decimal, so the signedness
// rules are in that function's comment.
struct Arg {
  using Value = std::variant<int32_t, uint32_t, bool, uint8_t, int64_t, uint64_t, float, double, std::string,
                             std::vector<uint8_t>, std::vector<int32_t>, std::vector<float>>;
  Value value;

  static Arg i32(int32_t v) { return Arg{Value(std::in_place_type<int32_t>, v)}; }
  static Arg u32(uint32_t v) { return Arg{Value(std::in_place_type<uint32_t>, v)}; }
  static Arg boolean(bool v) { return Arg{Value(std::in_place_type<bool>, v)}; }
  static Arg byte(uint8_t v) { return Arg{Value(std::in_place_type<uint8_t>, v)}; }
  static Arg i64(int64_t v) { return Arg{Value(std::in_place_type<int64_t>, v)}; }
  static Arg u64(uint64_t v) { return Arg{Value(std::in_place_type<uint64_t>, v)}; }
  static Arg f32(float v) { return Arg{Value(std::in_place_type<float>, v)}; }
  static Arg f64(double v) { return Arg{Value(std::in_place_type<double>, v)}; }
  static Arg string(std::string v) { return Arg{Value(std::in_place_type<std::string>, std::move(v))}; }
  static Arg bytes(std::vector<uint8_t> v) {
    return Arg{Value(std::in_place_type<std::vector<uint8_t>>, std::move(v))};
  }
  static Arg ints(std::vector<int32_t> v) {
    return Arg{Value(std::in_place_type<std::vector<int32_t>>, std::move(v))};
  }
  static Arg floats(std::vector<float> v) {
    return Arg{Value(std::in_place_type<std::vector<float>>, std::move(v))};
  }
};

// One generic call (type 0 to 8). arraySize is the `as=` field: 0 for every kind but
// the array kinds, 1 to the limit for those.
struct CallSpec {
  std::variant<uint32_t, ByName> target;
  ThreadContext thread = ThreadContext::Title;
  ReturnKind returns = ReturnKind::Void;
  size_t arraySize = 0;
  std::vector<Arg> args;
};

// ---- Building requests ------------------------------------------------------------

// The text of one argument: tag, separator, value, and the closing backslash, e.g.
// `1\5\`, `1/1\` (bool), `2/2\6869\`. Contract (D5):
//  - int32, bool and uint32 go through tag 1 as decimal; a uint32 above INT32_MAX is
//    sent as the equal negative int32, because the server reads it with sscanf %i
//    into an int. A bool is `1/0\` or `1/1\`. A byte is tag 4, decimal.
//  - int64 and uint64 are tag 8, decimal; a uint64 above INT64_MAX is sent as the
//    equal negative int64 for the same reason (sscanf %lli).
//  - float and double are tag 3 as `%.9g` and `%.17g` text in the C locale (never the
//    process locale). NaN and infinities are InvalidArgument.
//  - a string is tag 2: `2/<byte count>\<hex of the bytes>\`. A NUL byte or text that
//    is not valid UTF-8 is InvalidArgument (it becomes a C string on the console).
//    The empty string is `2/0\\`.
//  - a byte blob is tag 7, `7/<byte count>\<hex>\`; int and float arrays also use tag 7
//    with big-endian 4-byte elements (the document's tags 5 and 6 never go on the
//    wire). Any float bit pattern is allowed inside an array.
// A payload that cannot fit a line of maxCommandBytes is LimitExceeded without being
// encoded first.
UPDCLIENT_API Result<std::string> encodeArgument(const Arg &arg, size_t maxCommandBytes = kMaxCommandBytes);

// The command line of a call, without a terminator (section 2):
//   consolefeatures ver=2 type=<T>[ system][ module="<name>" ord=<n>] as=<n> params="A\<addr>\A\<argc>\<args>"
// Refused before anything is built: Uint64Array (Unsupported); a zero address (0 is
// the by-ordinal marker, calling it would be a bug), a module name that is empty or
// holds a space, a quote, a backslash or anything but printable ASCII, a wrong
// arraySize for the kind, or a bad argument (InvalidArgument); more than kMaxArgs
// arguments, an arraySize above maxArrayElements, or a line longer than
// maxCommandBytes (LimitExceeded).
UPDCLIENT_API Result<std::string> buildCommand(const CallSpec &spec, size_t maxCommandBytes = kMaxCommandBytes,
                                               size_t maxArrayElements = kMaxArrayElements);

// The command line of a system opcode (type 9 to 18): no `system`, `module` or `as`
// fields, e.g. `consolefeatures ver=2 type=10 params="A\0\A\0\"`. The address field is
// 0 except for ConstantMemorySet, where it carries the address to write (section 4.2).
// The arguments are encoded as for a call; the same limits apply. An unknown opcode is
// InvalidArgument.
UPDCLIENT_API Result<std::string> buildOpcodeCommand(Opcode opcode, std::span<const Arg> args = {},
                                                     uint32_t address = 0,
                                                     size_t maxCommandBytes = kMaxCommandBytes);

// line + "\r\n". The client sends CRLF; the server scans for the LF (section 1.3).
UPDCLIENT_API std::string formatLine(std::string_view line);

// ---- Reading replies --------------------------------------------------------------
//
// The parsers take one reply line with its terminator already removed (see
// stripTerminator). A reply has no status prefix; the caller has to look at
// isErrorLine and isDebugLine first. Anything a parser does not recognise is
// ErrorCode::Protocol with the (shortened) line in the message; D3 says the caller
// then closes the connection. Hex is read in either case, without a sign, a 0x prefix
// or white space.

// Removes one trailing "\r\n" or "\n". Contract: CRLF is what the server sends; a bare
// LF is tolerated.
UPDCLIENT_API std::string_view stripTerminator(std::string_view line) noexcept;

// A line starting `error=` is a failure of the call (section 6); errorText is what
// follows. A line containing `DEBUG` means JRPC is not installed (section 6, mirrors the
// JRPC.cs check), so a string result that holds that word cannot be told from it.
UPDCLIENT_API bool isErrorLine(std::string_view line) noexcept;
UPDCLIENT_API std::string_view errorText(std::string_view line) noexcept;
UPDCLIENT_API bool isDebugLine(std::string_view line) noexcept;

// What a successful call returns, by ReturnKind. Int, Byte and Int64 are the unsigned
// value (the typed helpers reinterpret it where the caller asks for a signed one).
using CallValue = std::variant<std::monostate, uint64_t, std::string, double, std::vector<int32_t>,
                               std::vector<double>, std::vector<uint8_t>>;

// type 0: `S_OK`, or the register in hex (D8: any hex, 1 to 16 digits).
UPDCLIENT_API Result<void> parseVoidReply(std::string_view line);
// type 1 and the opcodes that answer `%X` (9, 15, 16): 1 to 8 hex digits.
UPDCLIENT_API Result<uint32_t> parseIntReply(std::string_view line);
// type 4: `%X` of the low byte. Contract: 1 to 8 hex digits are accepted and the low
// 8 bits kept, because the document says "r3 low 8 bits" and does not say whether the
// server masks.
UPDCLIENT_API Result<uint8_t> parseByteReply(std::string_view line);
// type 8: `%llX`, 1 to 16 hex digits.
UPDCLIENT_API Result<uint64_t> parseInt64Reply(std::string_view line);
// type 3: `%f`, read with from_chars (locale independent). Six decimals, so a tiny
// float reads as 0 (documented). "inf" and "nan" spellings are accepted because a
// function can return them; out of range text is Protocol.
UPDCLIENT_API Result<double> parseFloatReply(std::string_view line);
// type 2: the whole line. Contract: a NUL, CR or LF inside is Protocol (the server
// prints a C string, and a stray CR means the framing is wrong).
UPDCLIENT_API Result<std::string> parseStringReply(std::string_view line);
// Opcode 13: `%d`, a signed decimal that fits int32.
UPDCLIENT_API Result<int32_t> parseDecimalReply(std::string_view line);

// types 5, 6, 7: `v,v,...,v;` with 1 to maxElements values and nothing after the `;`.
// Ints are signed decimal, floats `%f`, bytes `%X` (1 to 8 digits, low 8 bits kept, as
// for parseByteReply).
UPDCLIENT_API Result<std::vector<int32_t>> parseIntArrayReply(std::string_view line,
                                                              size_t maxElements = kMaxArrayElements);
UPDCLIENT_API Result<std::vector<double>> parseFloatArrayReply(std::string_view line,
                                                               size_t maxElements = kMaxArrayElements);
UPDCLIENT_API Result<std::vector<uint8_t>> parseByteArrayReply(std::string_view line,
                                                               size_t maxElements = kMaxArrayElements);

// The parser for `kind`. Contract: an array reply must hold exactly arraySize values
// (the server was asked for `as=arraySize`); arraySize is ignored for other kinds.
// Uint64Array is Unsupported.
UPDCLIENT_API Result<CallValue> parseReply(ReturnKind kind, std::string_view line, size_t arraySize = 0);

// GetCPUKey (opcode 10). The reply is two unpadded `%X` halves run together (D8), so
// the digits cannot be split reliably unless they are all there. Contract: exactly 16
// hex digits (two 32-bit halves, the document's example) or 32 (a full 128-bit key)
// give a key; any other length is Protocol with the raw text in the message rather
// than a guess. The plan text says 32 while the document's own example has 16, so
// both are accepted until a console shows which is real.
struct CpuKey {
  std::vector<uint8_t> bytes; // big-endian, 8 or 16 bytes

  std::string hex() const { return formatHex(bytes); }
  bool operator==(const CpuKey &) const = default;
};

UPDCLIENT_API Result<CpuKey> parseCpuKey(std::string_view line);

// ConsoleType (opcode 17): the motherboard family the server names.
enum class ConsoleType { Unknown, Xenon, Zephyr, Falcon, Jasper, Trinity, Corona };

UPDCLIENT_API std::string_view consoleTypeName(ConsoleType type) noexcept;
// Exactly one of the seven names (case sensitive); anything else is Protocol.
UPDCLIENT_API Result<ConsoleType> parseConsoleType(std::string_view line);

} // namespace updclient::jrpc
