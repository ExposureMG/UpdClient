#include "support/test_harness.hpp"

#include <updclient/core/error.hpp>
#include <updclient/core/expected.hpp>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

using namespace updclient;

#if defined(UPDCLIENT_USE_STD_EXPECTED)
static_assert(std::is_same_v<expected<int, Error>, std::expected<int, Error>>);
#else
static_assert(std::is_same_v<expected<int, Error>, tl::expected<int, Error>>,
              "Result<T> must not depend on the consumer's -std");
#endif
static_assert(std::is_same_v<Result<int>, expected<int, Error>>);
static_assert(std::is_same_v<Result<void>, expected<void, Error>>);
static_assert(std::is_move_constructible_v<Result<std::unique_ptr<int>>>);
static_assert(!std::is_copy_constructible_v<Result<std::unique_ptr<int>>>);
static_assert(std::is_default_constructible_v<Result<void>>);

namespace {

Result<int> parsePositive(int value) {
  if (value <= 0) return fail(ErrorCode::InvalidArgument, "not positive");
  return value;
}

Result<int> doubled(int value) {
  auto parsed = parsePositive(value);
  if (!parsed) return unexpected<Error>(parsed.error());
  return *parsed * 2;
}

} // namespace

TEST(Expected, ValueAccess) {
  Result<int> r = 41;
  REQUIRE(r.has_value());
  CHECK(static_cast<bool>(r));
  CHECK_EQ(*r, 41);
  CHECK_EQ(r.value(), 41);
  *r += 1;
  CHECK_EQ(*r, 42);
}

TEST(Expected, ErrorAccess) {
  Result<int> r = fail(ErrorCode::Timeout, "slow", 110);
  REQUIRE(!r.has_value());
  CHECK(!r);
  CHECK_EQ(r.error().code, ErrorCode::Timeout);
  CHECK_EQ(r.error().sysError, 110);
}

TEST(Expected, ValueOr) {
  Result<int> good = 5;
  Result<int> bad = fail(ErrorCode::Io, "x");
  CHECK_EQ(good.value_or(-1), 5);
  CHECK_EQ(bad.value_or(-1), -1);
}

TEST(Expected, ArrowOperator) {
  Result<std::string> r = std::string("hello");
  REQUIRE_OK(r);
  CHECK_EQ(r->size(), size_t{5});
}

TEST(Expected, VoidResult) {
  Result<void> ok;
  CHECK_OK(ok);
  Result<void> bad = fail(ErrorCode::Protocol, "broken");
  CHECK_ERR(bad, ErrorCode::Protocol);
}

TEST(Expected, UnexpectInPlaceConstruction) {
  Result<int> r(unexpect, Error{ErrorCode::Unsupported, "in place", 0});
  REQUIRE_ERR(r, ErrorCode::Unsupported);
  CHECK_EQ(r.error().message, std::string("in place"));
}

TEST(Expected, PropagationPattern) {
  CHECK_EQ(doubled(21).value_or(0), 42);
  CHECK_ERR(doubled(0), ErrorCode::InvalidArgument);
}

TEST(Expected, MoveOnlyPayload) {
  Result<std::unique_ptr<int>> r = std::make_unique<int>(9);
  REQUIRE_OK(r);
  auto owned = std::move(*r);
  REQUIRE(owned != nullptr);
  CHECK_EQ(*owned, 9);
}

TEST(Expected, CopyKeepsErrorIndependent) {
  Result<int> a = fail(ErrorCode::Io, "first");
  Result<int> b = a;
  b.error().message = "second";
  CHECK_EQ(a.error().message, std::string("first"));
  CHECK_EQ(b.error().message, std::string("second"));
}

TEST(Expected, ComparesWithErrorOnlyThroughAccessors) {
  Result<int> a = 1;
  Result<int> b = 1;
  CHECK(a.has_value() && b.has_value() && *a == *b);
}
