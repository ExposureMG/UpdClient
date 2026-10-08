#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/jrpc/protocol.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace updclient;
using namespace updclient::jrpc;

// The goldens are the document's own examples (JRPC_PROTOCOL.md section 7) and hand-written
// lines in the same shape; nothing here is produced by the code under test. Raw strings use a
// custom delimiter because the lines hold backslashes and quotes.

namespace {

CallSpec byAddress(uint32_t address, ReturnKind returns = ReturnKind::Void, std::vector<Arg> args = {}) {
  CallSpec spec;
  spec.target = address;
  spec.returns = returns;
  spec.args = std::move(args);
  return spec;
}

CallSpec byOrdinal(std::string module, uint32_t ordinal, ReturnKind returns = ReturnKind::Void,
                   std::vector<Arg> args = {}) {
  CallSpec spec;
  spec.target = ByName{std::move(module), ordinal};
  spec.returns = returns;
  spec.args = std::move(args);
  return spec;
}

// The text of one argument, or "" after recording a failure.
std::string encoded(const Arg &arg) {
  auto text = encodeArgument(arg);
  REQUIRE_OK(text);
  return *text;
}

std::string argLine(const Arg &arg) {
  auto line = buildCommand(byAddress(0x82000000, ReturnKind::Void, {arg}));
  REQUIRE_OK(line);
  return *line;
}

size_t lineLength(size_t blobBytes, size_t moduleChars) {
  CallSpec spec = byOrdinal(std::string(moduleChars, 'm'), 1, ReturnKind::Void,
                            {Arg::bytes(std::vector<uint8_t>(blobBytes, 0xAB))});
  auto line = buildCommand(spec, size_t{1} << 20);
  REQUIRE_OK(line);
  return line->size();
}

// A decoder for one argument written from the document's grammar (section 2), independent of
// the builder: tag, separator, then either `value\` or `count\hex\`.
struct DecodedArg {
  char tag = 0;
  char separator = 0;
  std::string value;       // scalar value, or the count of a length-prefixed argument
  std::string hex;         // the hex payload of a length-prefixed argument
};

bool decodeArg(std::string_view text, DecodedArg &out) {
  if (text.size() < 3 || text.back() != '\\') return false;
  out.tag = text[0];
  out.separator = text[1];
  text.remove_prefix(2);
  const size_t first = text.find('\\');
  if (first == std::string_view::npos) return false;
  out.value = std::string(text.substr(0, first));
  text.remove_prefix(first + 1);
  if (out.separator == '\\') return text.empty();
  if (out.separator != '/') return false;
  if (out.tag == '1') return text.empty(); // a bool is `1/<0|1>\`: no payload behind it
  const size_t second = text.find('\\');
  if (second != text.size() - 1) return false;
  out.hex = std::string(text.substr(0, second));
  return true;
}

} // namespace

// ---- Section 7 goldens ---------------------------------------------------------------

TEST(JrpcWire, Example71VoidCallAtAnAddress) {
  auto line = buildCommand(byAddress(0x82000000));
  REQUIRE_OK(line);
  CHECK_EQ(*line, std::string(R"j(consolefeatures ver=2 type=0 as=0 params="A\82000000\A\0\")j"));
}

TEST(JrpcWire, Example72IntCallWithTwoIntegers) {
  auto line = buildCommand(byAddress(0x82010000, ReturnKind::Int, {Arg::i32(5), Arg::i32(0x10)}));
  REQUIRE_OK(line);
  CHECK_EQ(*line, std::string(R"j(consolefeatures ver=2 type=1 as=0 params="A\82010000\A\2\1\5\1\16\")j"));
}

TEST(JrpcWire, Example73CallByOrdinalOnASystemThread) {
  CallSpec spec = byOrdinal("xam.xex", 0x1B4, ReturnKind::Int64);
  spec.thread = ThreadContext::System;
  auto line = buildCommand(spec);
  REQUIRE_OK(line);
  CHECK_EQ(*line, std::string(R"j(consolefeatures ver=2 type=8 system module="xam.xex" ord=436 as=0 params="A\0\A\0\")j"));
}

TEST(JrpcWire, Example74StringArgument) {
  auto line = buildCommand(byAddress(0x82020000, ReturnKind::Void, {Arg::string("hi")}));
  REQUIRE_OK(line);
  CHECK_EQ(*line, std::string(R"j(consolefeatures ver=2 type=0 as=0 params="A\82020000\A\1\2/2\6869\")j"));
}

TEST(JrpcWire, Example75XNotify) {
  const std::vector<Arg> args{Arg::string("Hello"), Arg::i32(0)};
  auto line = buildOpcodeCommand(Opcode::XNotify, args);
  REQUIRE_OK(line);
  CHECK_EQ(*line, std::string(R"j(consolefeatures ver=2 type=12 params="A\0\A\2\2/5\48656C6C6F\1\0\")j"));
}

TEST(JrpcWire, Example76GetCpuKeyAndItsReply) {
  auto line = buildOpcodeCommand(Opcode::GetCpuKey);
  REQUIRE_OK(line);
  CHECK_EQ(*line, std::string(R"j(consolefeatures ver=2 type=10 params="A\0\A\0\")j"));

  auto key = parseCpuKey("A1B2C3D4E5F60718");
  REQUIRE_OK(key);
  CHECK_EQ(key->bytes, (ut::Bytes{0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18}));
  CHECK_EQ(key->hex(), std::string("A1B2C3D4E5F60718"));
}

TEST(JrpcWire, SectionSevenReplies) {
  CHECK_OK(parseVoidReply("0"));
  CHECK_OK(parseVoidReply("S_OK"));
  auto forty2 = parseIntReply("2A");
  REQUIRE_OK(forty2);
  CHECK_EQ(*forty2, uint32_t{42});
  auto wide = parseInt64Reply("0000000248173A00");
  REQUIRE_OK(wide);
  CHECK_EQ(*wide, uint64_t{0x248173A00ull});
}

TEST(JrpcWire, FormatLineAddsCrLf) {
  CHECK_EQ(formatLine("Bye"), std::string("Bye\r\n"));
  CHECK_EQ(formatLine(""), std::string("\r\n"));
  CHECK_EQ(kBanner, std::string_view("JRPC2 connected"));
  CHECK_EQ(kBye, std::string_view("Bye"));
  CHECK_EQ(kJrpcPort, uint16_t{1409});
}

// ---- Every return tag ------------------------------------------------------------------

TEST(JrpcWire, EveryReturnKindSendsItsTypeNumber) {
  struct Case {
    ReturnKind kind;
    int type;
    size_t as;
  };
  const Case cases[] = {
      {ReturnKind::Void, 0, 0},       {ReturnKind::Int, 1, 0},        {ReturnKind::String, 2, 0},
      {ReturnKind::Float, 3, 0},      {ReturnKind::Byte, 4, 0},       {ReturnKind::IntArray, 5, 4},
      {ReturnKind::FloatArray, 6, 2}, {ReturnKind::ByteArray, 7, 8},  {ReturnKind::Int64, 8, 0},
  };
  for (const auto &c : cases) {
    CallSpec spec = byAddress(0x82000000, c.kind);
    spec.arraySize = c.as;
    auto line = buildCommand(spec);
    REQUIRE_OK(line);
    CHECK_EQ(*line, "consolefeatures ver=2 type=" + std::to_string(c.type) + " as=" + std::to_string(c.as) +
                        R"j( params="A\82000000\A\0\")j");
    CHECK_EQ(static_cast<int>(*returnTag(c.kind)), c.type);
  }
  CHECK(!returnTag(ReturnKind::Uint64Array).has_value());
}

TEST(JrpcWire, SixtyFourBitArrayReturnIsUnsupported) {
  CallSpec spec = byAddress(0x82000000, ReturnKind::Uint64Array);
  spec.arraySize = 2;
  CHECK_ERR(buildCommand(spec), ErrorCode::Unsupported);
}

TEST(JrpcWire, ArraySizeRules) {
  CallSpec spec = byAddress(0x82000000, ReturnKind::IntArray);
  spec.arraySize = 8;
  CHECK_OK(buildCommand(spec));
  spec.arraySize = 9;
  CHECK_ERR(buildCommand(spec), ErrorCode::LimitExceeded);
  // The limit is a parameter: a client may raise it once a console shows more is sent.
  auto raised = buildCommand(spec, kMaxCommandBytes, 16);
  REQUIRE_OK(raised);
  CHECK_EQ(*raised, std::string(R"j(consolefeatures ver=2 type=5 as=9 params="A\82000000\A\0\")j"));
  spec.arraySize = 0;
  CHECK_ERR(buildCommand(spec), ErrorCode::InvalidArgument);

  CallSpec scalar = byAddress(0x82000000, ReturnKind::Int);
  scalar.arraySize = 1;
  CHECK_ERR(buildCommand(scalar), ErrorCode::InvalidArgument);
  CHECK_EQ(kMaxArrayElements, size_t{8});
}

TEST(JrpcWire, SystemThreadComesBeforeTheModule) {
  CallSpec spec = byAddress(0x8201ABCD, ReturnKind::Int);
  spec.thread = ThreadContext::System;
  auto line = buildCommand(spec);
  REQUIRE_OK(line);
  CHECK_EQ(*line, std::string(R"j(consolefeatures ver=2 type=1 system as=0 params="A\8201ABCD\A\0\")j"));
}

TEST(JrpcWire, AddressIsUppercaseHexWithoutPrefixOrPadding) {
  auto small = buildCommand(byAddress(0x1F));
  REQUIRE_OK(small);
  CHECK_EQ(*small, std::string(R"j(consolefeatures ver=2 type=0 as=0 params="A\1F\A\0\")j"));
  auto top = buildCommand(byAddress(0xFFFFFFFF));
  REQUIRE_OK(top);
  CHECK_EQ(*top, std::string(R"j(consolefeatures ver=2 type=0 as=0 params="A\FFFFFFFF\A\0\")j"));
}

TEST(JrpcWire, TargetRules) {
  CHECK_ERR(buildCommand(byAddress(0)), ErrorCode::InvalidArgument);
  CHECK_ERR(buildCommand(byOrdinal("", 1)), ErrorCode::InvalidArgument);
  CHECK_ERR(buildCommand(byOrdinal("xam xex", 1)), ErrorCode::InvalidArgument);
  CHECK_ERR(buildCommand(byOrdinal("xam\".xex", 1)), ErrorCode::InvalidArgument);
  CHECK_ERR(buildCommand(byOrdinal("xam\\.xex", 1)), ErrorCode::InvalidArgument);
  CHECK_ERR(buildCommand(byOrdinal("xam\r.xex", 1)), ErrorCode::InvalidArgument);
  CHECK_ERR(buildCommand(byOrdinal(std::string("xam\0.xex", 8), 1)), ErrorCode::InvalidArgument);
  CHECK_ERR(buildCommand(byOrdinal("x\xC3\xA9.xex", 1)), ErrorCode::InvalidArgument);
  auto ordinalZero = buildCommand(byOrdinal("xboxkrnl.exe", 0));
  REQUIRE_OK(ordinalZero);
  CHECK_EQ(*ordinalZero, std::string(R"j(consolefeatures ver=2 type=0 module="xboxkrnl.exe" ord=0 as=0 params="A\0\A\0\")j"));
  auto big = buildCommand(byOrdinal("a", 0xFFFFFFFF));
  REQUIRE_OK(big);
  CHECK(big->find("ord=4294967295 ") != std::string::npos);
}

// ---- Argument encoding -----------------------------------------------------------------

TEST(JrpcWire, IntegerArguments) {
  CHECK_EQ(encoded(Arg::i32(0)), std::string(R"j(1\0\)j"));
  CHECK_EQ(encoded(Arg::i32(-1)), std::string(R"j(1\-1\)j"));
  CHECK_EQ(encoded(Arg::i32(std::numeric_limits<int32_t>::max())), std::string(R"j(1\2147483647\)j"));
  CHECK_EQ(encoded(Arg::i32(std::numeric_limits<int32_t>::min())), std::string(R"j(1\-2147483648\)j"));
  CHECK_EQ(encoded(Arg::u32(0)), std::string(R"j(1\0\)j"));
  CHECK_EQ(encoded(Arg::u32(0x7FFFFFFF)), std::string(R"j(1\2147483647\)j"));
}

TEST(JrpcWire, Uint32AboveInt32MaxGoesAsTheEqualNegativeInt) {
  CHECK_EQ(encoded(Arg::u32(0x80000000u)), std::string(R"j(1\-2147483648\)j"));
  CHECK_EQ(encoded(Arg::u32(0x80000001u)), std::string(R"j(1\-2147483647\)j"));
  CHECK_EQ(encoded(Arg::u32(0xFFFFFFFEu)), std::string(R"j(1\-2\)j"));
  CHECK_EQ(encoded(Arg::u32(0xFFFFFFFFu)), std::string(R"j(1\-1\)j"));
  CHECK_EQ(argLine(Arg::u32(0xFFFFFFFFu)), std::string(R"j(consolefeatures ver=2 type=0 as=0 params="A\82000000\A\1\1\-1\")j"));
}

TEST(JrpcWire, BooleanUsesTheSlashForm) {
  CHECK_EQ(encoded(Arg::boolean(true)), std::string(R"j(1/1\)j"));
  CHECK_EQ(encoded(Arg::boolean(false)), std::string(R"j(1/0\)j"));
}

TEST(JrpcWire, ByteIsTagFour) {
  CHECK_EQ(encoded(Arg::byte(0)), std::string(R"j(4\0\)j"));
  CHECK_EQ(encoded(Arg::byte(7)), std::string(R"j(4\7\)j"));
  CHECK_EQ(encoded(Arg::byte(255)), std::string(R"j(4\255\)j"));
}

TEST(JrpcWire, SixtyFourBitArgumentsAreTagEight) {
  CHECK_EQ(encoded(Arg::i64(0)), std::string(R"j(8\0\)j"));
  CHECK_EQ(encoded(Arg::i64(std::numeric_limits<int64_t>::min())), std::string(R"j(8\-9223372036854775808\)j"));
  CHECK_EQ(encoded(Arg::i64(std::numeric_limits<int64_t>::max())), std::string(R"j(8\9223372036854775807\)j"));
  CHECK_EQ(encoded(Arg::u64(0x248173A00ull)), std::string(R"j(8\9799416320\)j"));
  CHECK_EQ(encoded(Arg::u64(0x7FFFFFFFFFFFFFFFull)), std::string(R"j(8\9223372036854775807\)j"));
  // Above INT64_MAX the equal negative number, like the 32-bit case.
  CHECK_EQ(encoded(Arg::u64(0x8000000000000000ull)), std::string(R"j(8\-9223372036854775808\)j"));
  CHECK_EQ(encoded(Arg::u64(0xFFFFFFFFFFFFFFFFull)), std::string(R"j(8\-1\)j"));
}

TEST(JrpcWire, FloatsAreShortestSafeTextInTheCLocale) {
  CHECK_EQ(encoded(Arg::f32(1.5f)), std::string(R"j(3\1.5\)j"));
  CHECK_EQ(encoded(Arg::f32(-2.5f)), std::string(R"j(3\-2.5\)j"));
  CHECK_EQ(encoded(Arg::f32(0.0f)), std::string(R"j(3\0\)j"));
  CHECK_EQ(encoded(Arg::f32(0.1f)), std::string(R"j(3\0.100000001\)j"));      // %.9g
  CHECK_EQ(encoded(Arg::f32(1e10f)), std::string(R"j(3\1e+10\)j"));
  CHECK_EQ(encoded(Arg::f32(3.4028235e38f)), std::string(R"j(3\3.40282347e+38\)j"));
  CHECK_EQ(encoded(Arg::f32(1.17549435e-38f)), std::string(R"j(3\1.17549435e-38\)j"));
  CHECK_EQ(encoded(Arg::f64(0.1)), std::string(R"j(3\0.10000000000000001\)j")); // %.17g
  CHECK_EQ(encoded(Arg::f64(-0.0)), std::string(R"j(3\-0\)j"));
  CHECK_EQ(encoded(Arg::f64(1e22)), std::string(R"j(3\1e+22\)j"));
  CHECK_EQ(encoded(Arg::f64(5e-324)), std::string(R"j(3\4.9406564584124654e-324\)j"));
}

TEST(JrpcWire, NanAndInfinityAreRefused) {
  const float fnan = std::numeric_limits<float>::quiet_NaN();
  const float finf = std::numeric_limits<float>::infinity();
  const double dnan = std::numeric_limits<double>::quiet_NaN();
  const double dinf = std::numeric_limits<double>::infinity();
  CHECK_ERR(encodeArgument(Arg::f32(fnan)), ErrorCode::InvalidArgument);
  CHECK_ERR(encodeArgument(Arg::f32(finf)), ErrorCode::InvalidArgument);
  CHECK_ERR(encodeArgument(Arg::f32(-finf)), ErrorCode::InvalidArgument);
  CHECK_ERR(encodeArgument(Arg::f64(dnan)), ErrorCode::InvalidArgument);
  CHECK_ERR(encodeArgument(Arg::f64(dinf)), ErrorCode::InvalidArgument);
  CHECK_ERR(encodeArgument(Arg::f64(-dinf)), ErrorCode::InvalidArgument);
  CHECK_ERR(buildCommand(byAddress(0x82000000, ReturnKind::Void, {Arg::i32(1), Arg::f64(dnan)})),
            ErrorCode::InvalidArgument);
}

TEST(JrpcWire, StringArguments) {
  CHECK_EQ(encoded(Arg::string("hi")), std::string(R"j(2/2\6869\)j"));
  CHECK_EQ(encoded(Arg::string("Hello")), std::string(R"j(2/5\48656C6C6F\)j"));
  CHECK_EQ(encoded(Arg::string("a b\"c\\d")), std::string(R"j(2/7\61206222635C64\)j"));
  // The count is bytes, not characters.
  CHECK_EQ(encoded(Arg::string("\xC3\xA9")), std::string(R"j(2/2\C3A9\)j"));
  CHECK_EQ(encoded(Arg::string("\xF0\x9F\x98\x80")), std::string(R"j(2/4\F09F9880\)j"));
  CHECK_EQ(encoded(Arg::string("\xEF\xBF\xBD")), std::string(R"j(2/3\EFBFBD\)j"));
  CHECK_EQ(encoded(Arg::string("\xF4\x8F\xBF\xBF")), std::string(R"j(2/4\F48FBFBF\)j")); // U+10FFFF
}

TEST(JrpcWire, StringWithNulIsRefused) {
  CHECK_ERR(encodeArgument(Arg::string(std::string("a\0b", 3))), ErrorCode::InvalidArgument);
  CHECK_ERR(encodeArgument(Arg::string(std::string(1, '\0'))), ErrorCode::InvalidArgument);
  CHECK_ERR(buildCommand(byAddress(0x82000000, ReturnKind::Void, {Arg::string(std::string("x\0", 2))})),
            ErrorCode::InvalidArgument);
}

TEST(JrpcWire, StringThatIsNotUtf8IsRefused) {
  const char *bad[] = {
      "\x80",                 // lone continuation byte
      "\xC3",                 // truncated two-byte sequence
      "\xC3\x28",             // bad continuation
      "\xC0\x80",             // overlong NUL
      "\xC1\xBF",             // overlong
      "\xE0\x80\x80",         // overlong three-byte
      "\xE2\x82",             // truncated three-byte
      "\xED\xA0\x80",         // surrogate U+D800
      "\xED\xBF\xBF",         // surrogate U+DFFF
      "\xF0\x80\x80\x80",     // overlong four-byte
      "\xF4\x90\x80\x80",     // above U+10FFFF
      "\xF5\x80\x80\x80",     // invalid lead byte
      "\xFF",                 // invalid lead byte
      "ok\xFE",               // bad byte after valid text
  };
  for (const char *text : bad) {
    CHECK_ERR(encodeArgument(Arg::string(text)), ErrorCode::InvalidArgument);
  }
}

TEST(JrpcWire, EmptyStringAndEmptyBlob) {
  CHECK_EQ(encoded(Arg::string("")), std::string(R"j(2/0\\)j"));
  CHECK_EQ(encoded(Arg::bytes({})), std::string(R"j(7/0\\)j"));
  CHECK_EQ(encoded(Arg::ints({})), std::string(R"j(7/0\\)j"));
  CHECK_EQ(encoded(Arg::floats({})), std::string(R"j(7/0\\)j"));
  CHECK_EQ(argLine(Arg::string("")), std::string(R"j(consolefeatures ver=2 type=0 as=0 params="A\82000000\A\1\2/0\\")j"));
}

TEST(JrpcWire, BlobsAndArraysAreTagSevenWithByteCounts) {
  CHECK_EQ(encoded(Arg::bytes({0xDE, 0xAD, 0xBE, 0xEF})), std::string(R"j(7/4\DEADBEEF\)j"));
  CHECK_EQ(encoded(Arg::bytes({0x00, 0x0A, 0xFF})), std::string(R"j(7/3\000AFF\)j"));
  // int arrays: big-endian 4-byte elements, the count is in bytes
  CHECK_EQ(encoded(Arg::ints({1, -2})), std::string(R"j(7/8\00000001FFFFFFFE\)j"));
  CHECK_EQ(encoded(Arg::ints({std::numeric_limits<int32_t>::min(), 0x01020304})),
           std::string(R"j(7/8\8000000001020304\)j"));
  // float arrays: big-endian IEEE 754 singles
  CHECK_EQ(encoded(Arg::floats({1.0f, -2.5f})), std::string(R"j(7/8\3F800000C0200000\)j"));
  CHECK_EQ(encoded(Arg::floats({0.0f, -0.0f})), std::string(R"j(7/8\0000000080000000\)j"));
}

TEST(JrpcWire, FloatArraysCarryAnyBitPattern) {
  const float fnan = std::numeric_limits<float>::quiet_NaN();
  const float finf = std::numeric_limits<float>::infinity();
  CHECK_EQ(encoded(Arg::floats({fnan, finf})), std::string(R"j(7/8\7FC000007F800000\)j"));
}

TEST(JrpcWire, ArgumentsFollowTheDocumentGrammar) {
  const std::vector<Arg> args{Arg::i32(-7),
                              Arg::u32(0xDEADBEEF),
                              Arg::boolean(true),
                              Arg::byte(200),
                              Arg::i64(-5),
                              Arg::u64(0x123456789ull),
                              Arg::f32(0.25f),
                              Arg::f64(-1.5),
                              Arg::string("h\xC3\xA9llo"),
                              Arg::bytes(ut::patternBytes(33, 7)),
                              Arg::ints({1, 2, 3}),
                              Arg::floats({0.5f})};
  const char expectedTags[] = {'1', '1', '1', '4', '8', '8', '3', '3', '2', '7', '7', '7'};
  const char expectedSeparators[] = {'\\', '\\', '/', '\\', '\\', '\\', '\\', '\\', '/', '/', '/', '/'};
  for (size_t i = 0; i < args.size(); ++i) {
    DecodedArg decoded;
    const std::string text = encoded(args[i]);
    REQUIRE(decodeArg(text, decoded));
    CHECK_EQ(decoded.tag, expectedTags[i]);
    CHECK_EQ(decoded.separator, expectedSeparators[i]);
    if (decoded.separator == '/' && decoded.tag != '1') {
      // the prefix is the number of bytes behind the hex
      CHECK_EQ(decoded.hex.size(), 2 * std::stoul(decoded.value));
      CHECK(decoded.hex.find_first_not_of("0123456789ABCDEF") == std::string::npos);
    }
  }
  // The string decodes back to its bytes.
  DecodedArg text;
  REQUIRE(decodeArg(encoded(Arg::string("h\xC3\xA9llo")), text));
  CHECK_EQ(text.hex, std::string("68C3A96C6C6F"));
  CHECK_EQ(text.value, std::string("6"));

  auto line = buildCommand(byAddress(0x82000000, ReturnKind::Void, args));
  REQUIRE_OK(line);
  CHECK(line->find(R"j(\A\12\)j") != std::string::npos);
}

// ---- Limits ----------------------------------------------------------------------------

TEST(JrpcWire, ThirtySixThirtySevenAndThirtyEightArguments) {
  const auto argsOf = [](size_t count) {
    std::vector<Arg> args;
    for (size_t i = 0; i < count; ++i) args.push_back(Arg::i32(static_cast<int32_t>(i)));
    return args;
  };
  auto thirtySix = buildCommand(byAddress(0x82000000, ReturnKind::Void, argsOf(36)));
  REQUIRE_OK(thirtySix);
  CHECK(thirtySix->find(R"j(\A\36\)j") != std::string::npos);

  auto thirtySeven = buildCommand(byAddress(0x82000000, ReturnKind::Void, argsOf(37)));
  REQUIRE_OK(thirtySeven);
  CHECK(thirtySeven->find(R"j(\A\37\1\0\1\1\)j") != std::string::npos);
  CHECK(thirtySeven->ends_with(R"j(1\35\1\36\")j"));

  auto thirtyEight = buildCommand(byAddress(0x82000000, ReturnKind::Void, argsOf(38)));
  CHECK_ERR(thirtyEight, ErrorCode::LimitExceeded);
  CHECK_EQ(kMaxArgs, size_t{37});

  // opcodes count the same way
  const auto many = argsOf(38);
  CHECK_ERR(buildOpcodeCommand(Opcode::XNotify, many), ErrorCode::LimitExceeded);
  CHECK_OK(buildOpcodeCommand(Opcode::XNotify, std::span<const Arg>(many).first(37)));
}

TEST(JrpcWire, LineLimitIsExactlyEightThousandOneHundredNinetyOneBytes) {
  CHECK_EQ(kMaxCommandBytes, size_t{8191});
  // Find a call whose line is exactly 8191 bytes. The module name length moves the total by one,
  // the blob by two, so a combination always exists.
  size_t foundBlob = 0, foundModule = 0;
  for (size_t module = 1; module <= 4 && foundBlob == 0; ++module) {
    for (size_t blob = 3500; blob <= 4095; ++blob) {
      if (lineLength(blob, module) == 8191) {
        foundBlob = blob;
        foundModule = module;
        break;
      }
    }
  }
  REQUIRE(foundBlob != 0);

  CallSpec exact = byOrdinal(std::string(foundModule, 'm'), 1, ReturnKind::Void,
                             {Arg::bytes(std::vector<uint8_t>(foundBlob, 0xAB))});
  auto at = buildCommand(exact);
  REQUIRE_OK(at);
  CHECK_EQ(at->size(), size_t{8191});
  CHECK_EQ(formatLine(*at).size(), size_t{8193}); // still under the server's 8500-byte buffer

  // One byte more is over: a longer module name by one character.
  CallSpec over = byOrdinal(std::string(foundModule + 1, 'm'), 1, ReturnKind::Void,
                            {Arg::bytes(std::vector<uint8_t>(foundBlob, 0xAB))});
  CHECK_EQ(lineLength(foundBlob, foundModule + 1), size_t{8192});
  CHECK_ERR(buildCommand(over), ErrorCode::LimitExceeded);

  // The limit is a parameter, and applies to the line without its terminator.
  CHECK_OK(buildCommand(exact, 8191));
  CHECK_ERR(buildCommand(exact, 8190), ErrorCode::LimitExceeded);
  CHECK_OK(buildCommand(over, 8192));
}

TEST(JrpcWire, OversizedPayloadsAreRefusedWithoutBeingEncoded) {
  const std::vector<uint8_t> huge(size_t{1} << 20, 0x55);
  CHECK_ERR(encodeArgument(Arg::bytes(huge)), ErrorCode::LimitExceeded);
  CHECK_ERR(encodeArgument(Arg::string(std::string(size_t{1} << 20, 'a'))), ErrorCode::LimitExceeded);
  CHECK_ERR(encodeArgument(Arg::ints(std::vector<int32_t>(size_t{1} << 18, 1))), ErrorCode::LimitExceeded);
  CHECK_ERR(encodeArgument(Arg::floats(std::vector<float>(size_t{1} << 18, 1.0f))), ErrorCode::LimitExceeded);
  CHECK_ERR(buildCommand(byAddress(0x82000000, ReturnKind::Void, {Arg::bytes(huge)})), ErrorCode::LimitExceeded);
  // Many small arguments that add up past the limit, with a small limit.
  CHECK_ERR(buildCommand(byAddress(0x82000000, ReturnKind::Void, {Arg::bytes(std::vector<uint8_t>(40, 1)),
                                                                   Arg::bytes(std::vector<uint8_t>(40, 1))}),
                         200),
            ErrorCode::LimitExceeded);
}

// ---- Opcodes ---------------------------------------------------------------------------

TEST(JrpcWire, EveryOpcodeSendsItsTypeNumber) {
  struct Case {
    Opcode opcode;
    int type;
    std::string_view name;
    bool readOnly;
    bool silent;
  };
  const Case cases[] = {
      {Opcode::ResolveFunction, 9, "ResolveFunction", true, false},
      {Opcode::GetCpuKey, 10, "GetCpuKey", true, false},
      {Opcode::ShutDownConsole, 11, "ShutDownConsole", false, true},
      {Opcode::XNotify, 12, "XNotify", false, true},
      {Opcode::GetKernelVersion, 13, "GetKernelVersion", true, false},
      {Opcode::SetLeds, 14, "SetLeds", false, true},
      {Opcode::GetTemperature, 15, "GetTemperature", true, false},
      {Opcode::GetCurrentTitleId, 16, "GetCurrentTitleId", true, false},
      {Opcode::ConsoleType, 17, "ConsoleType", true, false},
      {Opcode::ConstantMemorySet, 18, "ConstantMemorySet", false, true},
  };
  for (const auto &c : cases) {
    auto line = buildOpcodeCommand(c.opcode);
    REQUIRE_OK(line);
    CHECK_EQ(*line, "consolefeatures ver=2 type=" + std::to_string(c.type) + R"j( params="A\0\A\0\")j");
    CHECK_EQ(static_cast<int>(c.opcode), c.type);
    CHECK_EQ(opcodeName(c.opcode), c.name);
    CHECK_EQ(isReadOnly(c.opcode), c.readOnly);
    CHECK_EQ(mayBeSilent(c.opcode), c.silent);
    CHECK(isKnownOpcode(c.opcode));
  }
}

TEST(JrpcWire, OpcodeWithArguments) {
  const std::vector<Arg> resolve{Arg::string("xam.xex"), Arg::i32(436)};
  auto resolveLine = buildOpcodeCommand(Opcode::ResolveFunction, resolve);
  REQUIRE_OK(resolveLine);
  CHECK_EQ(*resolveLine, std::string(R"j(consolefeatures ver=2 type=9 params="A\0\A\2\2/7\78616D2E786578\1\436\")j"));

  const std::vector<Arg> temperature{Arg::i32(3)};
  auto temperatureLine = buildOpcodeCommand(Opcode::GetTemperature, temperature);
  REQUIRE_OK(temperatureLine);
  CHECK_EQ(*temperatureLine, std::string(R"j(consolefeatures ver=2 type=15 params="A\0\A\1\1\3\")j"));

  const std::vector<Arg> leds{Arg::i32(1), Arg::i32(2), Arg::i32(3), Arg::i32(4)};
  auto ledLine = buildOpcodeCommand(Opcode::SetLeds, leds);
  REQUIRE_OK(ledLine);
  CHECK_EQ(*ledLine, std::string(R"j(consolefeatures ver=2 type=14 params="A\0\A\4\1\1\1\2\1\3\1\4\")j"));

  // opcode 18 carries its target address in the address field
  const std::vector<Arg> constant{Arg::u32(0xFFFFFFFF), Arg::i32(1), Arg::i32(2), Arg::i32(0), Arg::i32(0)};
  auto constantLine = buildOpcodeCommand(Opcode::ConstantMemorySet, constant, 0x82345678);
  REQUIRE_OK(constantLine);
  CHECK_EQ(*constantLine,
           std::string(R"j(consolefeatures ver=2 type=18 params="A\82345678\A\5\1\-1\1\1\1\2\1\0\1\0\")j"));
}

TEST(JrpcWire, UnknownOpcodesAreRefused) {
  CHECK_ERR(buildOpcodeCommand(static_cast<Opcode>(19)), ErrorCode::InvalidArgument);
  CHECK_ERR(buildOpcodeCommand(static_cast<Opcode>(8)), ErrorCode::InvalidArgument);
  CHECK_ERR(buildOpcodeCommand(static_cast<Opcode>(0)), ErrorCode::InvalidArgument);
  CHECK_ERR(buildOpcodeCommand(static_cast<Opcode>(255)), ErrorCode::InvalidArgument);
  CHECK(!isKnownOpcode(static_cast<Opcode>(19)));
}

// ---- Reply terminators and classification ----------------------------------------------

TEST(JrpcWire, StripTerminator) {
  CHECK_EQ(stripTerminator("2A\r\n"), std::string_view("2A"));
  CHECK_EQ(stripTerminator("2A\n"), std::string_view("2A"));
  CHECK_EQ(stripTerminator("2A"), std::string_view("2A"));
  CHECK_EQ(stripTerminator("2A\r"), std::string_view("2A\r"));
  CHECK_EQ(stripTerminator("2A\r\n\r\n"), std::string_view("2A\r\n"));
  CHECK_EQ(stripTerminator("\r\n"), std::string_view(""));
  CHECK_EQ(stripTerminator(""), std::string_view(""));
}

TEST(JrpcWire, ErrorAndDebugLines) {
  CHECK(isErrorLine("error=Could not resolve function address, params = x, 1"));
  CHECK(isErrorLine("error="));
  CHECK(!isErrorLine("ERROR=x"));
  CHECK(!isErrorLine(" error=x"));
  CHECK(!isErrorLine("error"));
  CHECK(!isErrorLine("errors=1"));
  CHECK(!isErrorLine(""));
  CHECK(!isErrorLine("2A"));
  CHECK_EQ(errorText("error=Version mismatch"), std::string_view("Version mismatch"));
  CHECK_EQ(errorText("error="), std::string_view(""));
  CHECK_EQ(errorText("2A"), std::string_view(""));

  CHECK(isDebugLine("DEBUG"));
  CHECK(isDebugLine("200- DEBUG build"));
  CHECK(isDebugLine("xxDEBUGxx"));
  CHECK(!isDebugLine("debug"));
  CHECK(!isDebugLine("DEBU"));
  CHECK(!isDebugLine(""));
}

// ---- Reply parsers: scalars ------------------------------------------------------------

TEST(JrpcWire, IntReplies) {
  CHECK_EQ(parseIntReply("0").value_or(9), uint32_t{0});
  CHECK_EQ(parseIntReply("2A").value_or(0), uint32_t{42});
  CHECK_EQ(parseIntReply("2a").value_or(0), uint32_t{42});
  CHECK_EQ(parseIntReply("FFFFFFFF").value_or(0), uint32_t{0xFFFFFFFF});
  CHECK_EQ(parseIntReply("00000001").value_or(0), uint32_t{1});
  CHECK_EQ(parseIntReply("80000000").value_or(0), uint32_t{0x80000000u});

  const char *bad[] = {"",         " ",          "2G",        "-1",        "+1",     "0x2A",   " 2A",
                       "2A ",      "2A\r",       "2\r\nA",    "100000000", "123456789", "FFFFFFFFF", "error=1",
                       "S_OK",     "1,2;",       "1.5",       "\xFF"};
  for (const char *text : bad) CHECK_ERR(parseIntReply(text), ErrorCode::Protocol);
  CHECK_ERR(parseIntReply(std::string("2\0A", 3)), ErrorCode::Protocol);
  CHECK_ERR(parseIntReply(std::string(1, '\0')), ErrorCode::Protocol);
}

TEST(JrpcWire, ByteReplies) {
  CHECK_EQ(parseByteReply("0").value_or(9), uint8_t{0});
  CHECK_EQ(parseByteReply("FF").value_or(0), uint8_t{0xFF});
  CHECK_EQ(parseByteReply("f").value_or(0), uint8_t{0x0F});
  CHECK_EQ(parseByteReply("07").value_or(9), uint8_t{7});
  // the low eight bits are the value, whatever the server leaves above them
  CHECK_EQ(parseByteReply("1AB").value_or(0), uint8_t{0xAB});
  CHECK_EQ(parseByteReply("FFFFFFF5").value_or(0), uint8_t{0xF5});
  CHECK_ERR(parseByteReply(""), ErrorCode::Protocol);
  CHECK_ERR(parseByteReply("123456789"), ErrorCode::Protocol);
  CHECK_ERR(parseByteReply("-1"), ErrorCode::Protocol);
  CHECK_ERR(parseByteReply("G"), ErrorCode::Protocol);
}

TEST(JrpcWire, Int64Replies) {
  CHECK_EQ(parseInt64Reply("0").value_or(9), uint64_t{0});
  CHECK_EQ(parseInt64Reply("FFFFFFFFFFFFFFFF").value_or(0), UINT64_MAX);
  CHECK_EQ(parseInt64Reply("8000000000000000").value_or(0), uint64_t{0x8000000000000000ull});
  CHECK_EQ(parseInt64Reply("1").value_or(0), uint64_t{1});
  CHECK_ERR(parseInt64Reply("10000000000000000"), ErrorCode::Protocol);
  CHECK_ERR(parseInt64Reply(""), ErrorCode::Protocol);
  CHECK_ERR(parseInt64Reply("-1"), ErrorCode::Protocol);
  CHECK_ERR(parseInt64Reply("0x1"), ErrorCode::Protocol);
  CHECK_ERR(parseInt64Reply("1 "), ErrorCode::Protocol);
}

TEST(JrpcWire, VoidReplies) {
  CHECK_OK(parseVoidReply("S_OK"));
  CHECK_OK(parseVoidReply("0"));
  CHECK_OK(parseVoidReply("80070005"));
  CHECK_OK(parseVoidReply("FFFFFFFFFFFFFFFF"));
  CHECK_ERR(parseVoidReply(""), ErrorCode::Protocol);
  CHECK_ERR(parseVoidReply("s_ok"), ErrorCode::Protocol);
  CHECK_ERR(parseVoidReply("S_OK "), ErrorCode::Protocol);
  CHECK_ERR(parseVoidReply("OK"), ErrorCode::Protocol);
  CHECK_ERR(parseVoidReply("10000000000000000"), ErrorCode::Protocol);
  CHECK_ERR(parseVoidReply("error=x"), ErrorCode::Protocol);
  CHECK_ERR(parseVoidReply("1,2;"), ErrorCode::Protocol);
}

TEST(JrpcWire, FloatReplies) {
  CHECK_EQ(parseFloatReply("1.500000").value_or(0), 1.5);
  CHECK_EQ(parseFloatReply("-2.250000").value_or(0), -2.25);
  CHECK_EQ(parseFloatReply("0.000000").value_or(1), 0.0);
  CHECK_EQ(parseFloatReply("123456789.123457").value_or(0), 123456789.123457);
  CHECK_EQ(parseFloatReply("100").value_or(0), 100.0);
  CHECK_EQ(parseFloatReply("1e3").value_or(0), 1000.0);
  CHECK(std::signbit(parseFloatReply("-0.000000").value_or(1.0)));
  auto inf = parseFloatReply("inf");
  REQUIRE_OK(inf);
  CHECK(std::isinf(*inf));
  auto nan = parseFloatReply("nan");
  REQUIRE_OK(nan);
  CHECK(std::isnan(*nan));
  // a very long %f of a huge double
  CHECK_EQ(parseFloatReply("1" + std::string(30, '0') + ".000000").value_or(0), 1e30);

  const char *bad[] = {"", " ", "+1.0", " 1.0", "1.0 ", "1.0\r", "1.0x", "abc", "1,5", "0x1p3", "--1", "1e999", "-1e999",
                       "1.0.0", "error=1", "1;"};
  for (const char *text : bad) CHECK_ERR(parseFloatReply(text), ErrorCode::Protocol);
}

TEST(JrpcWire, StringReplies) {
  CHECK_EQ(parseStringReply("hello world").value_or(""), std::string("hello world"));
  CHECK_EQ(parseStringReply("").value_or("x"), std::string());
  CHECK_EQ(parseStringReply("  padded  ").value_or(""), std::string("  padded  "));
  CHECK_EQ(parseStringReply("\xC3\xA9 and \xFF").value_or(""), std::string("\xC3\xA9 and \xFF"));
  CHECK_ERR(parseStringReply("a\rb"), ErrorCode::Protocol);
  CHECK_ERR(parseStringReply("a\nb"), ErrorCode::Protocol);
  CHECK_ERR(parseStringReply("trailing\r"), ErrorCode::Protocol);
  CHECK_ERR(parseStringReply(std::string("a\0b", 3)), ErrorCode::Protocol);
}

TEST(JrpcWire, DecimalReplies) {
  CHECK_EQ(parseDecimalReply("17559").value_or(0), int32_t{17559});
  CHECK_EQ(parseDecimalReply("0").value_or(9), int32_t{0});
  CHECK_EQ(parseDecimalReply("-1").value_or(0), int32_t{-1});
  CHECK_EQ(parseDecimalReply("2147483647").value_or(0), std::numeric_limits<int32_t>::max());
  CHECK_EQ(parseDecimalReply("-2147483648").value_or(0), std::numeric_limits<int32_t>::min());
  const char *bad[] = {"", "2147483648", "-2147483649", "99999999999999999999", "+5", " 5", "5 ", "5\r", "1A", "0x10",
                       "1.5", "-", "--1"};
  for (const char *text : bad) CHECK_ERR(parseDecimalReply(text), ErrorCode::Protocol);
}

// ---- Reply parsers: arrays -------------------------------------------------------------

TEST(JrpcWire, IntArrayReplies) {
  auto three = parseIntArrayReply("1,-2,3;");
  REQUIRE_OK(three);
  CHECK_EQ(*three, (std::vector<int32_t>{1, -2, 3}));
  auto one = parseIntArrayReply("5;");
  REQUIRE_OK(one);
  CHECK_EQ(*one, (std::vector<int32_t>{5}));
  auto eight = parseIntArrayReply("1,2,3,4,5,6,7,8;");
  REQUIRE_OK(eight);
  CHECK_EQ(eight->size(), size_t{8});
  auto extremes = parseIntArrayReply("-2147483648,2147483647;");
  REQUIRE_OK(extremes);
  CHECK_EQ((*extremes)[0], std::numeric_limits<int32_t>::min());
  CHECK_EQ((*extremes)[1], std::numeric_limits<int32_t>::max());

  // past the maximum
  CHECK_ERR(parseIntArrayReply("1,2,3,4,5,6,7,8,9;"), ErrorCode::Protocol);
  CHECK_OK(parseIntArrayReply("1,2,3,4,5,6,7,8,9;", 9));
  CHECK_ERR(parseIntArrayReply("1,2,3;", 2), ErrorCode::Protocol);
}

TEST(JrpcWire, FloatArrayReplies) {
  auto values = parseFloatArrayReply("1.500000,-2.000000,0.250000;");
  REQUIRE_OK(values);
  CHECK_EQ(*values, (std::vector<double>{1.5, -2.0, 0.25}));
  CHECK_ERR(parseFloatArrayReply("1.5,2.5,3.5,4.5,5.5,6.5,7.5,8.5,9.5;"), ErrorCode::Protocol);
  CHECK_ERR(parseFloatArrayReply("1.5,x;"), ErrorCode::Protocol);
}

TEST(JrpcWire, ByteArrayReplies) {
  auto values = parseByteArrayReply("0,FF,a,7F;");
  REQUIRE_OK(values);
  CHECK_EQ(*values, (ut::Bytes{0x00, 0xFF, 0x0A, 0x7F}));
  auto padded = parseByteArrayReply("00,01,FE;");
  REQUIRE_OK(padded);
  CHECK_EQ(*padded, (ut::Bytes{0x00, 0x01, 0xFE}));
  // low eight bits, as for a single byte
  auto wide = parseByteArrayReply("FFFFFFF5,1AB;");
  REQUIRE_OK(wide);
  CHECK_EQ(*wide, (ut::Bytes{0xF5, 0xAB}));
  CHECK_ERR(parseByteArrayReply("100000000;"), ErrorCode::Protocol);
  CHECK_ERR(parseByteArrayReply("G;"), ErrorCode::Protocol);
  CHECK_ERR(parseByteArrayReply("-1;"), ErrorCode::Protocol);
}

TEST(JrpcWire, ArrayRepliesMustBeExactlyShaped) {
  const char *bad[] = {
      "",          ";",         ",;",        "1,;",        ",1;",       "1,,2;",      "1;2;",     "1,2",      "1,2,",
      "1,2;x",     "1,2; ",     "1,2;\r",    " 1,2;",      "1, 2;",     "1 ,2;",      "1,2 ;",    "+1,2;",    "1.5,2;",
      "a,2;",      "1,2;;",     ";1,2",      "1,2\r;",     "1,\r2;",    "\r;",        "99999999999,1;",       "-,1;",
      "--1,2;",    "error=x;",
  };
  for (const char *text : bad) CHECK_ERR(parseIntArrayReply(text), ErrorCode::Protocol);
}

TEST(JrpcWire, EveryProperPrefixOfAnArrayReplyIsRefused) {
  const std::string valid = "12,-3,456,7;";
  REQUIRE_OK(parseIntArrayReply(valid));
  for (size_t length = 0; length < valid.size(); ++length) {
    CHECK_ERR(parseIntArrayReply(std::string_view(valid).substr(0, length)), ErrorCode::Protocol);
  }
  const std::string floats = "1.500000,-2.250000;";
  REQUIRE_OK(parseFloatArrayReply(floats));
  for (size_t length = 0; length < floats.size(); ++length) {
    CHECK_ERR(parseFloatArrayReply(std::string_view(floats).substr(0, length)), ErrorCode::Protocol);
  }
  const std::string bytes = "0,FF,1A;";
  REQUIRE_OK(parseByteArrayReply(bytes));
  for (size_t length = 0; length < bytes.size(); ++length) {
    CHECK_ERR(parseByteArrayReply(std::string_view(bytes).substr(0, length)), ErrorCode::Protocol);
  }
}

// ---- parseReply ------------------------------------------------------------------------

TEST(JrpcWire, ParseReplyDispatchesByKind) {
  {
    auto value = parseReply(ReturnKind::Void, "S_OK");
    REQUIRE_OK(value);
    CHECK(std::holds_alternative<std::monostate>(*value));
  }
  {
    auto value = parseReply(ReturnKind::Int, "2A");
    REQUIRE_OK(value);
    CHECK_EQ(std::get<uint64_t>(*value), uint64_t{42});
  }
  {
    auto value = parseReply(ReturnKind::Byte, "F5");
    REQUIRE_OK(value);
    CHECK_EQ(std::get<uint64_t>(*value), uint64_t{0xF5});
  }
  {
    auto value = parseReply(ReturnKind::Int64, "0000000248173A00");
    REQUIRE_OK(value);
    CHECK_EQ(std::get<uint64_t>(*value), uint64_t{0x248173A00ull});
  }
  {
    auto value = parseReply(ReturnKind::String, "Hello");
    REQUIRE_OK(value);
    CHECK_EQ(std::get<std::string>(*value), std::string("Hello"));
  }
  {
    auto value = parseReply(ReturnKind::Float, "1.500000");
    REQUIRE_OK(value);
    CHECK_EQ(std::get<double>(*value), 1.5);
  }
  {
    auto value = parseReply(ReturnKind::IntArray, "1,2,3;", 3);
    REQUIRE_OK(value);
    CHECK_EQ(std::get<std::vector<int32_t>>(*value), (std::vector<int32_t>{1, 2, 3}));
  }
  {
    auto value = parseReply(ReturnKind::FloatArray, "0.500000,1.000000;", 2);
    REQUIRE_OK(value);
    CHECK_EQ(std::get<std::vector<double>>(*value), (std::vector<double>{0.5, 1.0}));
  }
  {
    auto value = parseReply(ReturnKind::ByteArray, "DE,AD;", 2);
    REQUIRE_OK(value);
    CHECK_EQ(std::get<std::vector<uint8_t>>(*value), (ut::Bytes{0xDE, 0xAD}));
  }
}

TEST(JrpcWire, ParseReplyHoldsAnArrayToTheRequestedCount) {
  CHECK_ERR(parseReply(ReturnKind::IntArray, "1,2;", 3), ErrorCode::Protocol);
  CHECK_ERR(parseReply(ReturnKind::IntArray, "1,2,3,4;", 3), ErrorCode::Protocol);
  CHECK_ERR(parseReply(ReturnKind::ByteArray, "1;", 2), ErrorCode::Protocol);
  CHECK_ERR(parseReply(ReturnKind::FloatArray, "1.0,2.0,3.0;", 1), ErrorCode::Protocol);
  CHECK_ERR(parseReply(ReturnKind::IntArray, "1;", 0), ErrorCode::InvalidArgument);
  CHECK_ERR(parseReply(ReturnKind::Uint64Array, "1;", 1), ErrorCode::Unsupported);
  CHECK_ERR(parseReply(static_cast<ReturnKind>(99), "1"), ErrorCode::InvalidArgument);
}

TEST(JrpcWire, ParseReplyRefusesTheWrongShapeForEachKind) {
  CHECK_ERR(parseReply(ReturnKind::Int, "1,2;"), ErrorCode::Protocol);
  CHECK_ERR(parseReply(ReturnKind::Int, "1.5"), ErrorCode::Protocol);
  CHECK_ERR(parseReply(ReturnKind::Int64, "1,2;"), ErrorCode::Protocol);
  CHECK_ERR(parseReply(ReturnKind::Float, "abc"), ErrorCode::Protocol);
  CHECK_ERR(parseReply(ReturnKind::IntArray, "5", 1), ErrorCode::Protocol);
  CHECK_ERR(parseReply(ReturnKind::Void, "hello"), ErrorCode::Protocol);
  CHECK_ERR(parseReply(ReturnKind::String, "a\rb"), ErrorCode::Protocol);
}

// ---- CPU key and console type ----------------------------------------------------------

TEST(JrpcWire, CpuKeyFromSixteenOrThirtyTwoDigits) {
  auto sixteen = parseCpuKey("0123456789abcdef");
  REQUIRE_OK(sixteen);
  CHECK_EQ(sixteen->hex(), std::string("0123456789ABCDEF"));
  CHECK_EQ(sixteen->bytes.size(), size_t{8});

  auto thirtyTwo = parseCpuKey("00112233445566778899AABBCCDDEEFF");
  REQUIRE_OK(thirtyTwo);
  CHECK_EQ(thirtyTwo->bytes.size(), size_t{16});
  CHECK_EQ(thirtyTwo->bytes[0], uint8_t{0x00});
  CHECK_EQ(thirtyTwo->bytes[15], uint8_t{0xFF});
  CHECK(*sixteen == *sixteen);
  CHECK(!(*sixteen == *thirtyTwo));
}

TEST(JrpcWire, UnpaddedCpuKeyIsReportedWithItsText) {
  // A server that prints its halves unpadded: the digits cannot be split, so say so.
  for (const char *text : {"A1B2C3D4E5F6718", "1", "", "A1B2C3D4E5F607181", "00112233445566778899AABBCCDDEEF",
                           "00112233445566778899AABBCCDDEEFF0", "G1B2C3D4E5F60718", "A1B2C3D4E5F60718\r",
                           "A1B2C3D4 E5F60718", "0x1B2C3D4E5F60718"}) {
    auto key = parseCpuKey(text);
    REQUIRE(!key.has_value());
    CHECK_EQ(key.error().code, ErrorCode::Protocol);
  }
  auto key = parseCpuKey("A1B2C3D4E5F6718");
  REQUIRE(!key.has_value());
  CHECK(key.error().message.find("A1B2C3D4E5F6718") != std::string::npos);
}

TEST(JrpcWire, ConsoleTypeNames) {
  const std::pair<std::string_view, ConsoleType> names[] = {
      {"Xenon", ConsoleType::Xenon},     {"Zephyr", ConsoleType::Zephyr}, {"Falcon", ConsoleType::Falcon},
      {"Jasper", ConsoleType::Jasper},   {"Trinity", ConsoleType::Trinity}, {"Corona", ConsoleType::Corona},
      {"Unknown", ConsoleType::Unknown},
  };
  for (const auto &[name, type] : names) {
    auto parsed = parseConsoleType(name);
    REQUIRE_OK(parsed);
    CHECK(*parsed == type);
    CHECK_EQ(consoleTypeName(type), name);
  }
  for (const char *text : {"", "xenon", "XENON", "Xenon ", " Xenon", "Xenon\r", "Winchester", "Xenon,Zephyr", "0"}) {
    CHECK_ERR(parseConsoleType(text), ErrorCode::Protocol);
  }
}

// ---- Messages are bounded and safe -----------------------------------------------------

TEST(JrpcWire, ErrorMessagesShortenAndEscapeTheLine) {
  const std::string hostile = std::string("\x01\x1b[31m\"") + std::string(500, 'A') + "\r\n";
  auto result = parseIntReply(hostile);
  REQUIRE(!result.has_value());
  CHECK(result.error().message.size() < 200);
  CHECK(result.error().message.find('\x1b') == std::string::npos);
  CHECK(result.error().message.find('\r') == std::string::npos);
  CHECK(result.error().message.find("...") != std::string::npos);
}

// ---- Fuzz ------------------------------------------------------------------------------

namespace {

struct Lcg {
  uint32_t state;
  uint32_t next() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  }
};

} // namespace

TEST(JrpcWire, ParserFuzzNeverCrashesAndKeepsItsInvariants) {
  Lcg rng{12345};
  const std::string alphabet = std::string("0123456789ABCDEFabcdefxX,;-+. \r\n\\=\"") + std::string(1, '\0') + "\xFF" + "enrinfa";
  for (int round = 0; round < 6000; ++round) {
    std::string text;
    const size_t length = rng.next() % 40;
    for (size_t i = 0; i < length; ++i) text += alphabet[rng.next() % alphabet.size()];

    if (auto v = parseIntReply(text)) {
      CHECK(!text.empty() && text.size() <= 8);
      CHECK(text.find_first_not_of("0123456789ABCDEFabcdef") == std::string::npos);
    }
    if (auto v = parseInt64Reply(text)) CHECK(!text.empty() && text.size() <= 16);
    if (auto v = parseByteReply(text)) CHECK(!text.empty() && text.size() <= 8);
    if (auto v = parseVoidReply(text)) CHECK(text == "S_OK" || (!text.empty() && text.size() <= 16));
    if (auto v = parseDecimalReply(text)) CHECK(!text.empty());
    if (auto v = parseFloatReply(text)) CHECK(text.find_first_of("\r\n \0") == std::string::npos);
    if (auto v = parseStringReply(text)) {
      CHECK_EQ(*v, text);
      CHECK(text.find_first_of(std::string("\r\n\0", 3)) == std::string::npos);
    }
    if (auto v = parseIntArrayReply(text)) {
      CHECK(!v->empty() && v->size() <= kMaxArrayElements);
      CHECK(text.back() == ';');
    }
    if (auto v = parseFloatArrayReply(text)) CHECK(!v->empty() && v->size() <= kMaxArrayElements);
    if (auto v = parseByteArrayReply(text)) CHECK(!v->empty() && v->size() <= kMaxArrayElements);
    if (auto v = parseCpuKey(text)) CHECK(text.size() == 16 || text.size() == 32);
    (void)parseConsoleType(text);
    (void)isErrorLine(text);
    (void)isDebugLine(text);
    (void)stripTerminator(text);
    for (ReturnKind kind : {ReturnKind::Void, ReturnKind::Int, ReturnKind::String, ReturnKind::Float, ReturnKind::Byte,
                            ReturnKind::IntArray, ReturnKind::FloatArray, ReturnKind::ByteArray, ReturnKind::Int64,
                            ReturnKind::Uint64Array}) {
      auto reply = parseReply(kind, text, 1 + rng.next() % 8);
      if (!reply) CHECK(reply.error().code == ErrorCode::Protocol || reply.error().code == ErrorCode::Unsupported);
    }
  }
}

TEST(JrpcWire, ArrayParsersAgreeWithAnIndependentSplit) {
  // Build valid replies from random values and check the parse returns them.
  Lcg rng{777};
  for (int round = 0; round < 400; ++round) {
    const size_t count = 1 + rng.next() % 8;
    std::vector<int32_t> ints;
    ut::Bytes bytes;
    std::string intText, byteText;
    for (size_t i = 0; i < count; ++i) {
      const auto value = static_cast<int32_t>((rng.next() << 8) ^ rng.next());
      ints.push_back(value);
      intText += (i ? "," : "") + std::to_string(value);
      const auto byte = static_cast<uint8_t>(rng.next());
      bytes.push_back(byte);
      static constexpr char digits[] = "0123456789ABCDEF";
      byteText += (i ? "," : "");
      if (byte >= 16) byteText += digits[byte >> 4];
      byteText += digits[byte & 0xF];
    }
    intText += ";";
    byteText += ";";
    auto parsedInts = parseIntArrayReply(intText);
    REQUIRE_OK(parsedInts);
    CHECK_EQ(*parsedInts, ints);
    auto parsedBytes = parseByteArrayReply(byteText);
    REQUIRE_OK(parsedBytes);
    CHECK_EQ(*parsedBytes, bytes);
  }
}

TEST(JrpcWire, BuiltCommandsAlwaysHaveTheDocumentedShape) {
  // Random calls: the line is one line, starts with the verb, ends with the closing quote,
  // the argc field matches, and every argument decodes with the independent decoder.
  Lcg rng{2026};
  for (int round = 0; round < 300; ++round) {
    std::vector<Arg> args;
    const size_t count = rng.next() % 12;
    for (size_t i = 0; i < count; ++i) {
      switch (rng.next() % 7) {
      case 0: args.push_back(Arg::i32(static_cast<int32_t>(rng.next() * 7919u))); break;
      case 1: args.push_back(Arg::u32(rng.next() * 104729u)); break;
      case 2: args.push_back(Arg::boolean(rng.next() & 1)); break;
      case 3: args.push_back(Arg::f32(static_cast<float>(rng.next() % 100000) / 8.0f)); break;
      case 4: args.push_back(Arg::bytes(ut::patternBytes(rng.next() % 40, rng.next()))); break;
      case 5: args.push_back(Arg::u64((uint64_t{rng.next()} << 32) | rng.next())); break;
      default: args.push_back(Arg::string(std::string(rng.next() % 20, 'z'))); break;
      }
    }
    auto line = buildCommand(byAddress(0x82000000 + rng.next() % 0x1000, ReturnKind::Int, args));
    REQUIRE_OK(line);
    CHECK(line->starts_with("consolefeatures ver=2 type=1 as=0 params=\"A\\"));
    CHECK(line->ends_with("\\\""));
    CHECK(line->find_first_of("\r\n") == std::string::npos);
    CHECK(line->find("\\A\\" + std::to_string(count) + "\\") != std::string::npos);
    for (const Arg &arg : args) {
      DecodedArg decoded;
      CHECK(decodeArg(encoded(arg), decoded));
    }
  }
}
