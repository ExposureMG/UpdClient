#pragma once

#include "cpp_compat.hpp"

#if defined(_WIN32) || defined(__CYGWIN__)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")

    using socket_t = SOCKET;
    constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
    constexpr int SOCKET_ERROR_VAL = SOCKET_ERROR;

    inline bool socket_init() {
        WSADATA wsaData;
        return ::WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    }

    inline void socket_cleanup() {
        ::WSACleanup();
    }

    inline int close_socket(socket_t s) {
        return ::closesocket(s);
    }

    inline bool set_socket_timeout(socket_t s, int timeoutMs) {
        DWORD tv = static_cast<DWORD>(timeoutMs);
        int r1 = ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        int r2 = ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        return (r1 == 0 && r2 == 0);
    }

    inline int get_last_socket_error() {
        return ::WSAGetLastError();
    }
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <cerrno>

    using socket_t = int;
    constexpr socket_t INVALID_SOCKET_FD = -1;
    constexpr int SOCKET_ERROR_VAL = -1;

    inline bool socket_init() {
        return true;
    }

    inline void socket_cleanup() {
    }

    inline int close_socket(socket_t s) {
        return ::close(s);
    }

    inline bool set_socket_timeout(socket_t s, int timeoutMs) {
        struct timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int r1 = ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int r2 = ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        return (r1 == 0 && r2 == 0);
    }

    inline int get_last_socket_error() {
        return errno;
    }
#endif
