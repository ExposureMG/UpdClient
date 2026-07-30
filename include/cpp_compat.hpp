#pragma once

// DLL Export / Symbol Visibility Defines
#if defined(UPDCLIENT_STATIC)
    #define UPDCLIENT_API
#elif defined(_WIN32) || defined(__CYGWIN__)
    #if defined(UPDCLIENT_EXPORTS)
        #define UPDCLIENT_API __declspec(dllexport)
    #else
        #define UPDCLIENT_API __declspec(dllimport)
    #endif
#else
    #if defined(__GNUC__) && __GNUC__ >= 4
        #define UPDCLIENT_API __attribute__((visibility("default")))
    #else
        #define UPDCLIENT_API
    #endif
#endif

// C++23 / C++20 expected Abstraction
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
