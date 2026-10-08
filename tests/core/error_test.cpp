#include "support/test_harness.hpp"

#include <core/error.hpp>

#include <set>

using namespace updclient;

TEST(Error, MakeErrorSetsFields) {
  const Error e = makeError(ErrorCode::Io, "disk on fire", 5);
  CHECK_EQ(e.code, ErrorCode::Io);
  CHECK_EQ(e.message, std::string("disk on fire"));
  CHECK_EQ(e.sysError, 5);
}

TEST(Error, MakeErrorDefaultsSysErrorToZero) {
  const Error e = makeError(ErrorCode::Timeout, "slow");
  CHECK_EQ(e.sysError, 0);
}

TEST(Error, DefaultConstructedIsUnknown) {
  const Error e;
  CHECK_EQ(e.code, ErrorCode::Unknown);
  CHECK(e.message.empty());
  CHECK_EQ(e.sysError, 0);
}

TEST(Error, FailConvertsToAnyResult) {
  Result<int> a = fail(ErrorCode::Protocol, "bad frame", 7);
  REQUIRE_ERR(a, ErrorCode::Protocol);
  CHECK_EQ(a.error().message, std::string("bad frame"));
  CHECK_EQ(a.error().sysError, 7);

  Result<void> b = fail(ErrorCode::Unsupported, "nope");
  CHECK_ERR(b, ErrorCode::Unsupported);

  Result<std::string> c = fail(ErrorCode::Disconnected, "gone");
  CHECK_ERR(c, ErrorCode::Disconnected);
}

TEST(Error, ErrorCodeNamesMatchEnumerators) {
  CHECK_EQ(std::string(errorCodeName(ErrorCode::Unknown)), std::string("Unknown"));
  CHECK_EQ(std::string(errorCodeName(ErrorCode::InvalidArgument)), std::string("InvalidArgument"));
  CHECK_EQ(std::string(errorCodeName(ErrorCode::Unsupported)), std::string("Unsupported"));
  CHECK_EQ(std::string(errorCodeName(ErrorCode::NotConnected)), std::string("NotConnected"));
  CHECK_EQ(std::string(errorCodeName(ErrorCode::ConnectFailed)), std::string("ConnectFailed"));
  CHECK_EQ(std::string(errorCodeName(ErrorCode::Timeout)), std::string("Timeout"));
  CHECK_EQ(std::string(errorCodeName(ErrorCode::Disconnected)), std::string("Disconnected"));
  CHECK_EQ(std::string(errorCodeName(ErrorCode::Io)), std::string("Io"));
  CHECK_EQ(std::string(errorCodeName(ErrorCode::Protocol)), std::string("Protocol"));
  CHECK_EQ(std::string(errorCodeName(ErrorCode::LimitExceeded)), std::string("LimitExceeded"));
  CHECK_EQ(std::string(errorCodeName(ErrorCode::Cancelled)), std::string("Cancelled"));
  CHECK_EQ(std::string(errorCodeName(ErrorCode::AlreadyExists)), std::string("AlreadyExists"));
}

TEST(Error, ErrorCodeNamesAreDistinct) {
  std::set<std::string> names;
  for (int i = 0; i <= static_cast<int>(ErrorCode::AlreadyExists); ++i) {
    names.insert(errorCodeName(static_cast<ErrorCode>(i)));
  }
  CHECK_EQ(names.size(), static_cast<size_t>(ErrorCode::AlreadyExists) + 1);
  // Appended: every earlier value keeps its number.
  CHECK_EQ(static_cast<int>(ErrorCode::Cancelled), 10);
  CHECK_EQ(static_cast<int>(ErrorCode::AlreadyExists), 11);
}

TEST(Error, UnknownEnumValueFallsBackToUnknown) {
  CHECK_EQ(std::string(errorCodeName(static_cast<ErrorCode>(9999))), std::string("Unknown"));
}

TEST(Error, FormatErrorWithoutOsError) {
  CHECK_EQ(formatError(makeError(ErrorCode::Timeout, "read timed out")), std::string("Timeout: read timed out"));
}

TEST(Error, FormatErrorWithOsError) {
  CHECK_EQ(formatError(makeError(ErrorCode::Io, "send failed", 32)),
           std::string("Io: send failed (os error 32)"));
}

TEST(Error, FormatErrorWithEmptyMessage) {
  CHECK_EQ(formatError(makeError(ErrorCode::Unknown, "")), std::string("Unknown: "));
}
