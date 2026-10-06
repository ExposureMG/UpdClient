#include "support/test_harness.hpp"

#include <updclient/core/path.hpp>

using namespace updclient;

TEST(Path, AsciiRoundTrips) {
  CHECK_EQ(pathToUtf8(pathFromUtf8("dir/file.bin")), pathToUtf8(std::filesystem::path("dir") / "file.bin"));
}

TEST(Path, Utf8RoundTrips) {
  const std::string text = "jos\xC3\xA9-\xE6\x97\xA5\xE6\x9C\xAC.bin";
  const auto path = pathFromUtf8(text);
  CHECK_EQ(pathToUtf8(path), text);
  CHECK_EQ(pathToUtf8(path.filename()), text);
}

TEST(Path, EmptyStaysEmpty) {
  CHECK(pathFromUtf8("").empty());
  CHECK_EQ(pathToUtf8({}), std::string());
}
