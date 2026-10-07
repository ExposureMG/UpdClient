#pragma once

#if defined(UPDCLIENT_STATIC)
#define UPDCLIENT_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#if defined(UPDCLIENT_EXPORTS)
#define UPDCLIENT_API __declspec(dllexport)
#else
#define UPDCLIENT_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define UPDCLIENT_API __attribute__((visibility("default")))
#else
#define UPDCLIENT_API
#endif
