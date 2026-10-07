#pragma once

#include <version>

// The expected type is part of the library's ABI (Result<T> appears in every
// exported signature), so the choice must not depend on the consumer's -std. The
// default is tl::expected. CMake defines UPDCLIENT_USE_STD_EXPECTED, publicly, only
// when the library itself was built against std::expected.
#if defined(UPDCLIENT_USE_STD_EXPECTED)
#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202211L
#error "UPDCLIENT_USE_STD_EXPECTED requires a standard library with std::expected (C++23)"
#endif
#include <expected>
namespace updclient {
using std::expected;
using std::unexpect;
using std::unexpected;
} // namespace updclient
#else
#include <tl/expected.hpp>
namespace updclient {
using tl::expected;
using tl::unexpect;
using tl::unexpected;
} // namespace updclient
#endif
