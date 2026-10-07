#include "support/test_harness.hpp"
#include "support/test_util.hpp"

#include <protocols/xbdm/protocol.hpp>

#include <chrono>
#include <cstdint>
#include <string>

using namespace updclient;
using namespace updclient::xbdm;

TEST(XbdmProtocol, StatusLineTakesTheCodeAndTheTextAfterOneSpace) {
  auto ok = parseStatusLine("200- OK");
  REQUIRE(ok.has_value());
  CHECK_EQ(ok->code, 200);
  CHECK_EQ(ok->text, std::string("OK"));
  CHECK(ok->isSuccess());
  CHECK(!ok->isRefusal());

  auto name = parseStatusLine("200- My Devkit  ");
  REQUIRE(name.has_value());
  CHECK_EQ(name->text, std::string("My Devkit  "));

  auto bare = parseStatusLine("410-already exists");
  REQUIRE(bare.has_value());
  CHECK_EQ(bare->code, 410);
  CHECK_EQ(bare->text, std::string("already exists"));
  CHECK(bare->isRefusal());

  auto empty = parseStatusLine("202-");
  REQUIRE(empty.has_value());
  CHECK_EQ(empty->text, std::string());

  auto twoSpaces = parseStatusLine("200-  x");
  REQUIRE(twoSpaces.has_value());
  CHECK_EQ(twoSpaces->text, std::string(" x"));
}

TEST(XbdmProtocol, StatusLineRejectsAnythingElse) {
  CHECK(!parseStatusLine("").has_value());
  CHECK(!parseStatusLine("200").has_value());
  CHECK(!parseStatusLine("200 OK").has_value());
  CHECK(!parseStatusLine("2a0- x").has_value());
  CHECK(!parseStatusLine("abc").has_value());
  CHECK(!parseStatusLine(" 200- OK").has_value());
  auto odd = parseStatusLine("500- internal");
  REQUIRE(odd.has_value());
  CHECK(!odd->isSuccess());
  CHECK(!odd->isRefusal());
}

TEST(XbdmProtocol, ParamsReadQuotedValuesNumbersAndFlags) {
  const auto p = parseParams(
      "name=\"My File.txt\" sizehi=0x0 SIZELO=0x0004f000 createhi=0x01d11fb5 directory ReadOnly");
  CHECK(!p.malformed);
  REQUIRE(p.find("name") != nullptr);
  CHECK_EQ(*p.find("name"), std::string("My File.txt"));
  REQUIRE(p.find("sizelo") != nullptr);
  CHECK_EQ(*p.find("sizelo"), std::string("0x0004f000"));
  CHECK(p.find("SIZELO") == nullptr);
  CHECK(p.hasFlag("directory"));
  CHECK(p.hasFlag("readonly"));
  CHECK(!p.hasFlag("hidden"));
}

TEST(XbdmProtocol, QuotedValuesEndAtTheNextQuoteAndKeepBackslashes) {
  const auto p = parseParams("name=\"HDD:\\\" newname=\"a b\"");
  REQUIRE(p.find("name") != nullptr);
  CHECK_EQ(*p.find("name"), std::string("HDD:\\"));
  REQUIRE(p.find("newname") != nullptr);
  CHECK_EQ(*p.find("newname"), std::string("a b"));
  CHECK(!p.malformed);
}

TEST(XbdmProtocol, ParamsDropOneTrailingCommaFromUnquotedValues) {
  const auto p = parseParams("pitch=0x00000c00 offsety=0x0, framebuffersize=0x10,, name=\"a,\"");
  CHECK_EQ(*p.find("offsety"), std::string("0x0"));
  CHECK_EQ(*p.find("framebuffersize"), std::string("0x10,"));
  CHECK_EQ(*p.find("name"), std::string("a,"));
}

TEST(XbdmProtocol, ParamsFlagBrokenQuotes) {
  auto open = parseParams("name=\"unterminated sizelo=0x1");
  CHECK(open.malformed);
  CHECK_EQ(*open.find("name"), std::string("unterminated sizelo=0x1"));

  auto glued = parseParams("name=\"a\"b sizelo=0x1");
  CHECK(glued.malformed);
  CHECK_EQ(*glued.find("name"), std::string("a"));
  CHECK_EQ(*glued.find("sizelo"), std::string("0x1"));

  auto noKey = parseParams("=0x1 a=1");
  CHECK(noKey.malformed);
  CHECK_EQ(*noKey.find("a"), std::string("1"));

  auto spaces = parseParams("   a=1\t\tb=2   ");
  CHECK(!spaces.malformed);
  CHECK_EQ(*spaces.find("b"), std::string("2"));
  CHECK(parseParams("").values.empty());
}

TEST(XbdmProtocol, ParamsKeepTheFirstOfTwoEqualKeys) {
  const auto p = parseParams("a=1 a=2");
  CHECK_EQ(*p.find("a"), std::string("1"));
  CHECK_EQ(p.values.size(), size_t{2});
}

TEST(XbdmProtocol, NumbersInEveryFormTheConsoleUses) {
  CHECK_EQ(parseNumber32("0x2a").value_or(0), uint32_t{42});
  CHECK_EQ(parseNumber32("0X2A").value_or(0), uint32_t{42});
  CHECK_EQ(parseNumber32("0x0000002A").value_or(0), uint32_t{42});
  CHECK_EQ(parseNumber32("0q2a").value_or(0), uint32_t{42});
  CHECK_EQ(parseNumber32("42").value_or(0), uint32_t{42});
  CHECK_EQ(parseNumber32("0").value_or(1), uint32_t{0});
  CHECK_EQ(parseNumber32("0xFFFFFFFF").value_or(0), uint32_t{0xFFFFFFFF});
  CHECK_EQ(parseNumber32("0x00000000FFFFFFFF").value_or(0), uint32_t{0xFFFFFFFF});
  CHECK_EQ(parseNumber64("0q0123456789abcdef").value_or(0), uint64_t{0x0123456789abcdefull});
  CHECK_EQ(parseNumber64("18446744073709551615").value_or(0), UINT64_MAX);
}

TEST(XbdmProtocol, NumbersRefuseJunkAndOverflow) {
  CHECK(!parseNumber32("").has_value());
  CHECK(!parseNumber32("0x").has_value());
  CHECK(!parseNumber32("0x1g").has_value());
  CHECK(!parseNumber32("-1").has_value());
  CHECK(!parseNumber32("1 ").has_value());
  CHECK(!parseNumber32("0x100000000").has_value());
  CHECK(!parseNumber32("4294967296").has_value());
  CHECK(!parseNumber64("0x10000000000000000").has_value());
  CHECK(!parseNumber64("18446744073709551616").has_value());
  CHECK(!parseNumber64("0b101").has_value());
}

TEST(XbdmProtocol, FormatNumberIsLowerCaseHexWithoutPadding) {
  CHECK_EQ(formatNumber(0), std::string("0x0"));
  CHECK_EQ(formatNumber(42), std::string("0x2a"));
  CHECK_EQ(formatNumber(0x82000000u), std::string("0x82000000"));
  CHECK_EQ(formatNumber(0x140000000ull), std::string("0x140000000"));
  CHECK_EQ(parseNumber64(formatNumber(UINT64_MAX)).value_or(0), UINT64_MAX);
}

TEST(XbdmProtocol, FileTimeConvertsToUnixTimeAndBack) {
  // 2015-12-02 11:43:04 UTC, from the dirlist example of the spec.
  const uint64_t fileTime = joinHalves(0x01d11fb5, 0x59683c00);
  auto point = fileTimeToTimePoint(fileTime);
  REQUIRE(point.has_value());
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(point->time_since_epoch()).count();
  CHECK_EQ(seconds, static_cast<int64_t>(fileTime / 10000000) - 11644473600ll);
  CHECK_EQ(timePointToFileTime(*point).value_or(0), fileTime);

  auto epoch = fileTimeToTimePoint(kFileTimeUnixEpoch);
  REQUIRE(epoch.has_value());
  CHECK_EQ(epoch->time_since_epoch().count(), int64_t{0});

  auto start = fileTimeToTimePoint(0);
  REQUIRE(start.has_value());
  CHECK(start->time_since_epoch().count() < 0);
  CHECK_EQ(timePointToFileTime(*start).value_or(1), uint64_t{0});

  CHECK(!fileTimeToTimePoint(UINT64_MAX).has_value());
  CHECK(!timePointToFileTime(*start - FileTimeTicks(1)).has_value());
}

TEST(XbdmProtocol, NamePackets) {
  CHECK_EQ(makeWildcardQuery(), (ut::Bytes{3, 0}));
  auto lookup = makeNameLookup("Dev1");
  REQUIRE_OK(lookup);
  CHECK_EQ(*lookup, (ut::Bytes{1, 4, 'D', 'e', 'v', '1'}));
  CHECK_ERR(makeNameLookup(""), ErrorCode::InvalidArgument);
  CHECK_ERR(makeNameLookup(std::string(256, 'a')), ErrorCode::InvalidArgument);
  CHECK_ERR(makeNameLookup("tab\there"), ErrorCode::InvalidArgument);
  CHECK_OK(makeNameLookup(std::string(255, 'a')));
}

TEST(XbdmProtocol, NameRepliesAreValidated) {
  CHECK_EQ(parseNameReply(ut::Bytes{2, 3, 'B', 'o', 'x'}).value_or(""), std::string("Box"));
  CHECK_EQ(parseNameReply(ut::Bytes{2, 2, 'B', 'o', 'x'}).value_or(""), std::string("Bo"));
  CHECK(!parseNameReply(ut::Bytes{1, 3, 'B', 'o', 'x'}).has_value());
  CHECK(!parseNameReply(ut::Bytes{2, 4, 'B', 'o', 'x'}).has_value());
  CHECK(!parseNameReply(ut::Bytes{2, 0}).has_value());
  CHECK(!parseNameReply(ut::Bytes{2}).has_value());
  CHECK(!parseNameReply(ut::Bytes{}).has_value());
  CHECK(!parseNameReply(ut::Bytes{2, 2, 'B', 0}).has_value());
  CHECK(!parseNameReply(ut::Bytes{2, 1, 0xC3}).has_value());
}
