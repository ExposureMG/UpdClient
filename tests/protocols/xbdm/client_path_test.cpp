#include "support/test_harness.hpp"

#include <protocols/xbdm/path.hpp>

#include <string>
#include <vector>

using namespace updclient;
using namespace updclient::xbdm;

namespace {

std::string console(std::string_view slash) {
  auto r = toConsolePath(slash);
  return r ? *r : "<error: " + r.error().message + ">";
}

std::string slash(std::string_view consolePath) {
  auto r = fromConsolePath(consolePath);
  return r ? *r : "<error: " + r.error().message + ">";
}

} // namespace

TEST(XbdmPath, SlashFormMapsToConsolePaths) {
  CHECK_EQ(console("/HDD/dir/file.bin"), std::string("HDD:\\dir\\file.bin"));
  CHECK_EQ(console("/HDD"), std::string("HDD:\\"));
  CHECK_EQ(console("/HDD/"), std::string("HDD:\\"));
  CHECK_EQ(console("/DEVKIT/a/"), std::string("DEVKIT:\\a"));
  CHECK_EQ(console("/E/x"), std::string("E:\\x"));

  CHECK_EQ(slash("HDD:\\dir\\file.bin"), std::string("/HDD/dir/file.bin"));
  CHECK_EQ(slash("HDD:\\"), std::string("/HDD"));
  CHECK_EQ(slash("HDD:"), std::string("/HDD"));
  CHECK_EQ(slash("HDD:\\dir\\"), std::string("/HDD/dir"));
}

TEST(XbdmPath, OddNamesRoundTrip) {
  const std::vector<std::string> names = {
      "a b",        " leading space", "trailing space ", "trailing dot.", "...", ".hidden", "x..y",
      "#$%&'()+,;", "=@[]^_`{}~!",   "UPPER lower",     "a",             "0", std::string(42, 'n'),
      "name.with.many.dots.tar.gz", "-", "~1",
  };
  for (const auto &name : names) {
    const std::string slashPath = "/HDD/folder/" + name;
    auto consolePath = toConsolePath(slashPath);
    REQUIRE_OK(consolePath);
    CHECK_EQ(*consolePath, "HDD:\\folder\\" + name);
    CHECK_EQ(slash(*consolePath), slashPath);
    CHECK_OK(validateName(name));
    auto quoted = quoteValue(*consolePath);
    REQUIRE_OK(quoted);
    CHECK_EQ(*quoted, "\"" + *consolePath + "\"");
    CHECK_EQ(nameOf(*consolePath).value_or(""), name);
    CHECK_EQ(parentOf(*consolePath).value_or(""), std::string("HDD:\\folder"));
    CHECK_EQ(joinPath("HDD:\\folder", name).value_or(""), *consolePath);
  }
}

TEST(XbdmPath, InvalidNamesAreRefused) {
  const std::vector<std::string> bad = {
      "",     ".",     "..",    "a\"b",  "a\\b", "a/b",   "a:b",      "a*b", "a?b",
      "a<b",  "a>b",   "a|b",   "a\rb",  "a\nb", std::string("a\0b", 3), "tab\t", "\x7f",
      "caf\xc3\xa9",
  };
  for (const auto &name : bad) {
    CHECK_ERR(validateName(name), ErrorCode::InvalidArgument);
    // In the slash form "a/b" is two valid names; "/HDD/" alone is the root.
    if (name.find('/') == std::string::npos) {
      if (!name.empty()) CHECK_ERR(toConsolePath("/HDD/" + name), ErrorCode::InvalidArgument);
      CHECK_ERR(toConsolePath("/HDD/" + name + "/x"), ErrorCode::InvalidArgument);
    }
    CHECK_ERR(joinPath("HDD:\\", name), ErrorCode::InvalidArgument);
  }
}

TEST(XbdmPath, MalformedPathsAreRefused) {
  for (const char *path : {"", "HDD", "HDD/a", ":\\a", "HDD:a", "HDD:\\\\", "HDD:\\a\\\\", "HDD:\\a\\\\b",
                           "HDD:\\a/b", "H D:\\a", "HDD:\\a:b", "\\Device\\Harddisk0\\x"}) {
    CHECK_ERR(canonicalPath(path), ErrorCode::InvalidArgument);
  }
  for (const char *path : {"", "HDD", "/", "//", "/HDD//a", "/HDD/a//", "/HDD:/a", "/H D/a", "/HDD/./a"}) {
    CHECK_ERR(toConsolePath(path), ErrorCode::InvalidArgument);
  }
  CHECK_ERR(canonicalPath(std::string(36, 'A') + ":\\"), ErrorCode::InvalidArgument);
  CHECK_OK(canonicalPath(std::string(35, 'A') + ":\\"));
}

TEST(XbdmPath, CanonicalFormAndHelpers) {
  CHECK_EQ(canonicalPath("HDD:").value_or(""), std::string("HDD:\\"));
  CHECK_EQ(canonicalPath("HDD:\\").value_or(""), std::string("HDD:\\"));
  CHECK_EQ(canonicalPath("HDD:\\a\\").value_or(""), std::string("HDD:\\a"));
  CHECK_EQ(canonicalPath("hdd:\\A").value_or(""), std::string("hdd:\\A"));
  CHECK(isDriveRoot("HDD:\\"));
  CHECK(isDriveRoot("HDD:"));
  CHECK(!isDriveRoot("HDD:\\a"));
  CHECK(!isDriveRoot("junk"));
  CHECK_EQ(driveOf("GAME:\\a\\b").value_or(""), std::string("GAME"));
  CHECK_EQ(parentOf("HDD:\\a").value_or(""), std::string("HDD:\\"));
  CHECK_EQ(parentOf("HDD:\\a\\b\\c").value_or(""), std::string("HDD:\\a\\b"));
  CHECK_ERR(parentOf("HDD:\\"), ErrorCode::InvalidArgument);
  CHECK_ERR(nameOf("HDD:\\"), ErrorCode::InvalidArgument);
  CHECK_EQ(joinPath("HDD:\\", "x").value_or(""), std::string("HDD:\\x"));
  CHECK_EQ(joinPath("HDD:", "x").value_or(""), std::string("HDD:\\x"));
}

TEST(XbdmPath, QuotingRefusesWhatCouldEndTheValueOrTheLine) {
  CHECK_EQ(quoteValue("HDD:\\").value_or(""), std::string("\"HDD:\\\""));
  CHECK_EQ(quoteValue("").value_or("x"), std::string("\"\""));
  CHECK_EQ(quoteValue("a b").value_or(""), std::string("\"a b\""));
  for (const auto &value : std::vector<std::string>{"a\"b", "a\r", "a\n", std::string("a\0", 2), "\x01",
                                                    "\x7f", "\xff"}) {
    CHECK_ERR(quoteValue(value), ErrorCode::InvalidArgument);
  }
}

TEST(XbdmPath, DriveNames) {
  CHECK_OK(validateDriveName("HDD"));
  CHECK_OK(validateDriveName("DEVKIT"));
  CHECK_OK(validateDriveName("E"));
  CHECK_OK(validateDriveName("Usb0"));
  CHECK_ERR(validateDriveName(""), ErrorCode::InvalidArgument);
  CHECK_ERR(validateDriveName("HDD:"), ErrorCode::InvalidArgument);
  CHECK_ERR(validateDriveName("H_D"), ErrorCode::InvalidArgument);
}
