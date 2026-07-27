#pragma once

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
