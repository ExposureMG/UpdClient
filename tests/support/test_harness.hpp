#pragma once

#include <updclient/core/error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Minimal self-contained test framework: a registry, CHECK/REQUIRE macros, SKIP and
// expected-failure (XFAIL) tests. No third-party dependencies.
//
//   TEST(Suite, Name) { CHECK_EQ(1 + 1, 2); REQUIRE(ptr != nullptr); }
//   XFAIL_TEST(Suite, Name, "why this is known to fail") { ... }
//
// CHECK records a failure and continues; REQUIRE records it and ends the test.
// An XFAIL test must fail; if it passes (XPASS) the run fails so the marker gets removed.
namespace ut {

struct AbortSignal {};

struct SkipSignal {
  std::string reason;
};

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
  const char *file = "";
  int line = 0;
  bool expectFail = false;
  std::string xfailReason;

  std::string fullName() const { return suite + "." + name; }
};

class Registry {
public:
  static Registry &instance() {
    static Registry registry;
    return registry;
  }
  void add(TestCase test) { tests_.push_back(std::move(test)); }
  const std::vector<TestCase> &tests() const { return tests_; }

private:
  std::vector<TestCase> tests_;
};

struct Registrar {
  Registrar(const char *suite, const char *name, void (*body)(), const char *file, int line, bool expectFail,
            const char *reason) {
    TestCase test;
    test.suite = suite;
    test.name = name;
    test.body = body;
    test.file = file;
    test.line = line;
    test.expectFail = expectFail;
    test.xfailReason = reason ? reason : "";
    Registry::instance().add(std::move(test));
  }
};

struct Context {
  std::mutex mutex;
  std::vector<std::string> failures;
  size_t checks = 0;
};

inline Context &context() {
  static Context ctx;
  return ctx;
}

inline void resetContext() {
  auto &ctx = context();
  std::lock_guard<std::mutex> lock(ctx.mutex);
  ctx.failures.clear();
  ctx.checks = 0;
}

inline void countCheck() {
  auto &ctx = context();
  std::lock_guard<std::mutex> lock(ctx.mutex);
  ++ctx.checks;
}

inline void recordFailure(const char *file, int line, const std::string &what, const std::string &detail) {
  std::string text = std::string(file) + ":" + std::to_string(line) + ": " + what;
  if (!detail.empty()) text += "\n      " + detail;
  auto &ctx = context();
  std::lock_guard<std::mutex> lock(ctx.mutex);
  ctx.failures.push_back(std::move(text));
}

[[noreturn]] inline void skip(std::string reason) {
  throw SkipSignal{std::move(reason)};
}

namespace detail {

template <class T, class = void> struct IsStreamable : std::false_type {};
template <class T>
struct IsStreamable<T, std::void_t<decltype(std::declval<std::ostream &>() << std::declval<const T &>())>>
    : std::true_type {};

template <class T, class = void> struct HasCount : std::false_type {};
template <class T> struct HasCount<T, std::void_t<decltype(std::declval<const T &>().count())>> : std::true_type {};

inline std::string quote(std::string_view text) {
  std::string out = "\"";
  for (char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c == '"') out += "\\\"";
    else if (u < 0x20 || u >= 0x7f) {
      static constexpr char digits[] = "0123456789abcdef";
      out += "\\x";
      out += digits[u >> 4];
      out += digits[u & 0xF];
    } else out += c;
  }
  return out + "\"";
}

inline std::string hexPreview(std::span<const uint8_t> bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  constexpr size_t kMaxShown = 48;
  std::string out = "bytes[" + std::to_string(bytes.size()) + "]";
  if (bytes.empty()) return out;
  out += ":";
  for (size_t i = 0; i < bytes.size() && i < kMaxShown; ++i) {
    out += ' ';
    out += digits[bytes[i] >> 4];
    out += digits[bytes[i] & 0xF];
  }
  if (bytes.size() > kMaxShown) out += " ...";
  return out;
}

} // namespace detail

inline std::string display(bool value) { return value ? "true" : "false"; }
inline std::string display(std::string_view value) { return detail::quote(value); }
inline std::string display(const std::string &value) { return detail::quote(value); }
inline std::string display(const char *value) { return value ? detail::quote(value) : "nullptr"; }
inline std::string display(updclient::ErrorCode code) { return updclient::errorCodeName(code); }
inline std::string display(const std::vector<uint8_t> &value) { return detail::hexPreview(value); }
inline std::string display(std::nullptr_t) { return "nullptr"; }

template <class T> std::string display(const T &value) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(static_cast<std::underlying_type_t<T>>(value)));
  } else if constexpr (std::is_integral_v<T>) {
    if constexpr (std::is_signed_v<T>) return std::to_string(static_cast<long long>(value));
    else return std::to_string(static_cast<unsigned long long>(value));
  } else if constexpr (detail::HasCount<T>::value) {
    return std::to_string(static_cast<long long>(value.count())) + " ticks";
  } else if constexpr (detail::IsStreamable<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<unprintable>";
  }
}

namespace detail {
template <class T>
inline constexpr bool IsCmpInteger = std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> &&
                                     !std::is_same_v<T, wchar_t> && !std::is_same_v<T, char8_t> &&
                                     !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t>;
} // namespace detail

template <class A, class B> bool equal(const A &a, const B &b) {
  if constexpr (detail::IsCmpInteger<A> && detail::IsCmpInteger<B>) {
    return std::cmp_equal(a, b);
  } else if constexpr (std::is_integral_v<A> && std::is_integral_v<B> && !std::is_same_v<A, bool> &&
                       !std::is_same_v<B, bool>) {
    return static_cast<long long>(a) == static_cast<long long>(b);
  } else {
    return a == b;
  }
}

template <class A, class B>
bool checkEqual(const char *file, int line, const char *exprA, const char *exprB, const A &a, const B &b,
                bool fatal) {
  countCheck();
  if (equal(a, b)) return true;
  recordFailure(file, line, std::string(fatal ? "REQUIRE_EQ(" : "CHECK_EQ(") + exprA + ", " + exprB + ")",
                "left:  " + display(a) + "\n      right: " + display(b));
  if (fatal) throw AbortSignal{};
  return false;
}

template <class A, class B>
bool checkNotEqual(const char *file, int line, const char *exprA, const char *exprB, const A &a, const B &b) {
  countCheck();
  if (!equal(a, b)) return true;
  recordFailure(file, line, std::string("CHECK_NE(") + exprA + ", " + exprB + ")",
                "both are: " + display(a));
  return false;
}

inline bool checkCondition(const char *file, int line, const char *expr, bool value, bool fatal,
                           const std::string &note = {}) {
  countCheck();
  if (value) return true;
  recordFailure(file, line, std::string(fatal ? "REQUIRE(" : "CHECK(") + expr + ")", note);
  if (fatal) throw AbortSignal{};
  return false;
}

template <class R> bool checkOk(const char *file, int line, const char *expr, const R &result, bool fatal) {
  countCheck();
  if (result.has_value()) return true;
  recordFailure(file, line, std::string(fatal ? "REQUIRE_OK(" : "CHECK_OK(") + expr + ")",
                "unexpected error: " + updclient::formatError(result.error()));
  if (fatal) throw AbortSignal{};
  return false;
}

template <class R>
bool checkError(const char *file, int line, const char *expr, const R &result, updclient::ErrorCode code,
                bool fatal) {
  countCheck();
  const char *label = fatal ? "REQUIRE_ERR(" : "CHECK_ERR(";
  if (result.has_value()) {
    recordFailure(file, line, std::string(label) + expr + ")",
                  std::string("expected error ") + updclient::errorCodeName(code) + " but the call succeeded");
    if (fatal) throw AbortSignal{};
    return false;
  }
  if (result.error().code != code) {
    recordFailure(file, line, std::string(label) + expr + ")",
                  std::string("expected error ") + updclient::errorCodeName(code) +
                      " but got: " + updclient::formatError(result.error()));
    if (fatal) throw AbortSignal{};
    return false;
  }
  return true;
}

enum class Outcome { Pass, Fail, Skip, XFail, XPass };

struct TestResult {
  Outcome outcome = Outcome::Pass;
  std::vector<std::string> failures;
  std::string note;
  size_t checks = 0;
  std::chrono::milliseconds elapsed{0};
};

inline TestResult runOne(const TestCase &test) {
  resetContext();
  TestResult result;
  const auto start = std::chrono::steady_clock::now();
  bool skipped = false;
  try {
    test.body();
  } catch (const AbortSignal &) {
  } catch (const SkipSignal &signal) {
    skipped = true;
    result.note = signal.reason;
  } catch (const std::exception &e) {
    recordFailure(test.file, test.line, "unexpected exception", e.what());
  } catch (...) {
    recordFailure(test.file, test.line, "unexpected exception of unknown type", "");
  }
  result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

  auto &ctx = context();
  {
    std::lock_guard<std::mutex> lock(ctx.mutex);
    result.failures = ctx.failures;
    result.checks = ctx.checks;
  }

  const bool failed = !result.failures.empty();
  if (skipped && !failed) result.outcome = Outcome::Skip;
  else if (test.expectFail) result.outcome = failed ? Outcome::XFail : Outcome::XPass;
  else result.outcome = failed ? Outcome::Fail : Outcome::Pass;
  return result;
}

} // namespace ut

#define UT_CONCAT_INNER(a, b) a##b
#define UT_CONCAT(a, b) UT_CONCAT_INNER(a, b)

#define UT_REGISTER_TEST(suite, name, expectFail, reason)                                                    \
  static void UT_CONCAT(ut_test_##suite##_##name##_, __LINE__)();                                            \
  static const ::ut::Registrar UT_CONCAT(ut_registrar_##suite##_##name##_, __LINE__)(                        \
      #suite, #name, &UT_CONCAT(ut_test_##suite##_##name##_, __LINE__), __FILE__, __LINE__, expectFail,      \
      reason);                                                                                               \
  static void UT_CONCAT(ut_test_##suite##_##name##_, __LINE__)()

#define TEST(suite, name) UT_REGISTER_TEST(suite, name, false, nullptr)
#define XFAIL_TEST(suite, name, reason) UT_REGISTER_TEST(suite, name, true, reason)

#define CHECK(cond) ::ut::checkCondition(__FILE__, __LINE__, #cond, static_cast<bool>(cond), false)
#define REQUIRE(cond) ::ut::checkCondition(__FILE__, __LINE__, #cond, static_cast<bool>(cond), true)
#define CHECK_MSG(cond, note) ::ut::checkCondition(__FILE__, __LINE__, #cond, static_cast<bool>(cond), false, note)

#define CHECK_EQ(a, b) ::ut::checkEqual(__FILE__, __LINE__, #a, #b, (a), (b), false)
#define REQUIRE_EQ(a, b) ::ut::checkEqual(__FILE__, __LINE__, #a, #b, (a), (b), true)
#define CHECK_NE(a, b) ::ut::checkNotEqual(__FILE__, __LINE__, #a, #b, (a), (b))

#define CHECK_OK(result) ::ut::checkOk(__FILE__, __LINE__, #result, (result), false)
#define REQUIRE_OK(result) ::ut::checkOk(__FILE__, __LINE__, #result, (result), true)
#define CHECK_ERR(result, code) ::ut::checkError(__FILE__, __LINE__, #result, (result), (code), false)
#define REQUIRE_ERR(result, code) ::ut::checkError(__FILE__, __LINE__, #result, (result), (code), true)

#define SKIP(reason) ::ut::skip(reason)
