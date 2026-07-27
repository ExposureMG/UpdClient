#pragma once

#if __has_include(<expected>) && defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
#include <expected>
namespace updclient {
using std::expected;
using std::unexpected;
using std::unexpect;
}
#else
#include <tl/expected.hpp>
namespace updclient {
using tl::expected;
using tl::unexpected;
using tl::unexpect;
}
#endif
