#include "support/test_harness.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *label(ut::Outcome outcome) {
  switch (outcome) {
  case ut::Outcome::Pass: return " OK    ";
  case ut::Outcome::Fail: return " FAIL  ";
  case ut::Outcome::Skip: return " SKIP  ";
  case ut::Outcome::XFail: return " XFAIL ";
  case ut::Outcome::XPass: return " XPASS ";
  }
  return " ?     ";
}

} // namespace

int main(int argc, char **argv) {
  std::string filter;
  bool list = false;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--list") == 0) list = true;
    else if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
    else if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) filter = argv[++i];
    else {
      std::fprintf(stderr, "usage: %s [--list] [--verbose] [--filter <text>]\n", argv[0]);
      return 2;
    }
  }

  std::vector<const ut::TestCase *> selected;
  for (const auto &test : ut::Registry::instance().tests()) {
    if (filter.empty() || test.fullName().find(filter) != std::string::npos) selected.push_back(&test);
  }
  std::sort(selected.begin(), selected.end(), [](const ut::TestCase *a, const ut::TestCase *b) {
    return a->fullName() < b->fullName();
  });

  if (list) {
    for (const auto *test : selected) std::printf("%s\n", test->fullName().c_str());
    return 0;
  }
  if (selected.empty()) {
    std::fprintf(stderr, "no tests match '%s'\n", filter.c_str());
    return 2;
  }

  size_t passed = 0, failed = 0, skipped = 0, xfailed = 0, xpassed = 0, checks = 0;
  std::vector<std::string> problems;
  for (const auto *test : selected) {
    const ut::TestResult result = ut::runOne(*test);
    checks += result.checks;
    switch (result.outcome) {
    case ut::Outcome::Pass: ++passed; break;
    case ut::Outcome::Fail: ++failed; problems.push_back(test->fullName()); break;
    case ut::Outcome::Skip: ++skipped; break;
    case ut::Outcome::XFail: ++xfailed; break;
    case ut::Outcome::XPass: ++xpassed; problems.push_back(test->fullName() + " (unexpected pass)"); break;
    }

    const bool quiet = result.outcome == ut::Outcome::Pass && !verbose;
    if (!quiet) {
      std::printf("[%s] %s (%lld ms)\n", label(result.outcome), test->fullName().c_str(),
                  static_cast<long long>(result.elapsed.count()));
    }
    if (result.outcome == ut::Outcome::Skip) std::printf("          skipped: %s\n", result.note.c_str());
    if (result.outcome == ut::Outcome::XPass) {
      std::printf("          %s:%d: expected to fail (%s) but passed; remove the XFAIL marker\n", test->file,
                  test->line, test->xfailReason.c_str());
    }
    if (result.outcome == ut::Outcome::XFail) {
      std::printf("          known issue (%s:%d): %s\n", test->file, test->line, test->xfailReason.c_str());
      if (verbose) {
        for (const auto &failure : result.failures) std::printf("          %s\n", failure.c_str());
      }
    }
    if (result.outcome == ut::Outcome::Fail) {
      for (const auto &failure : result.failures) std::printf("    %s\n", failure.c_str());
    }
    std::fflush(stdout);
  }

  std::printf("\n%zu tests, %zu checks: %zu passed, %zu failed, %zu skipped, %zu expected failures, %zu unexpected passes\n",
              selected.size(), checks, passed, failed, skipped, xfailed, xpassed);
  if (!problems.empty()) {
    std::printf("problems:\n");
    for (const auto &name : problems) std::printf("  %s\n", name.c_str());
  }
  return problems.empty() ? 0 : 1;
}
