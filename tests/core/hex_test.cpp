#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <core/hex.hpp>

#include <array>

using namespace updclient;

TEST(Hex, FormatEmpty) {
  CHECK_EQ(formatHex({}), std::string());
}

TEST(Hex, FormatIsUppercaseAndZeroPadded) {
  const std::array<uint8_t, 5> bytes{0x00, 0x0A, 0xAB, 0xFF, 0x07};
  CHECK_EQ(formatHex(bytes), std::string("000AABFF07"));
}

TEST(Hex, ParseAcceptsEitherCase) {
  std::array<uint8_t, 4> out{};
  REQUIRE_OK(parseHex("deadBEEF", out));
  CHECK_EQ(out[0], 0xDE);
  CHECK_EQ(out[1], 0xAD);
  CHECK_EQ(out[2], 0xBE);
  CHECK_EQ(out[3], 0xEF);
}

TEST(Hex, ParseEmptyIntoEmpty) {
  std::array<uint8_t, 0> out{};
  CHECK_OK(parseHex("", out));
}

TEST(Hex, RoundTripEveryByteValue) {
  ut::Bytes all(256);
  for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<uint8_t>(i);
  const std::string text = formatHex(all);
  CHECK_EQ(text.size(), size_t{512});

  ut::Bytes back(256);
  REQUIRE_OK(parseHex(text, back));
  CHECK_EQ(back, all);
}

TEST(Hex, RoundTripVariousLengths) {
  for (size_t length = 0; length <= 70; ++length) {
    const auto data = ut::patternBytes(length, static_cast<uint32_t>(length + 1));
    ut::Bytes back(length);
    const auto parsed = parseHex(formatHex(data), back);
    CHECK_OK(parsed);
    CHECK_EQ(back, data);
  }
}

TEST(Hex, ParseRejectsWrongLength) {
  std::array<uint8_t, 2> out{};
  CHECK_ERR(parseHex("", out), ErrorCode::InvalidArgument);
  CHECK_ERR(parseHex("ABC", out), ErrorCode::InvalidArgument);
  CHECK_ERR(parseHex("ABCDE", out), ErrorCode::InvalidArgument);
  CHECK_ERR(parseHex("ABCDEF00", out), ErrorCode::InvalidArgument);
}

TEST(Hex, ParseRejectsNonHexCharacters) {
  std::array<uint8_t, 2> out{};
  CHECK_ERR(parseHex("ABCG", out), ErrorCode::InvalidArgument);
  CHECK_ERR(parseHex("GBCD", out), ErrorCode::InvalidArgument);
  CHECK_ERR(parseHex("AB CD", out), ErrorCode::InvalidArgument);
  CHECK_ERR(parseHex("AB C", out), ErrorCode::InvalidArgument);
  CHECK_ERR(parseHex("0x12", out), ErrorCode::InvalidArgument);
  CHECK_ERR(parseHex("-1ab", out), ErrorCode::InvalidArgument);
  CHECK_ERR(parseHex(std::string_view("AB\0D", 4), out), ErrorCode::InvalidArgument);
}

TEST(Hex, ParseRejectsNonAsciiBytes) {
  std::array<uint8_t, 2> out{};
  CHECK_ERR(parseHex("\xC3\xA9" "AB", out), ErrorCode::InvalidArgument);
}

TEST(Hex, ParseErrorNamesOffset) {
  std::array<uint8_t, 3> out{};
  const auto r = parseHex("AABBZZ", out);
  REQUIRE_ERR(r, ErrorCode::InvalidArgument);
  CHECK(r.error().message.find('4') != std::string::npos);
}
