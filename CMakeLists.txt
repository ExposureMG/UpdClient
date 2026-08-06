cmake_minimum_required(VERSION 3.20)
project(UpdClient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# 1. CLI11
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
)

# 2. spdlog
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
)

# 3. nlohmann_json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)

# 4. tl::expected for C++20 compatibility
FetchContent_Declare(
    expected
    GIT_REPOSITORY https://github.com/TartanLlama/expected.git
    GIT_TAG        v1.1.0
)

set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

if(NOT TARGET CLI11)
    FetchContent_MakeAvailable(cli11)
endif()
if(NOT TARGET spdlog)
    FetchContent_MakeAvailable(spdlog)
endif()
if(NOT TARGET nlohmann_json)
    FetchContent_MakeAvailable(json)
endif()
if(NOT TARGET expected)
    FetchContent_MakeAvailable(expected)
endif()

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(UPDCLIENT_BUILD_CLI "Build CLI executable" ON)

# Library target
add_library(updclient_lib
    src/network/tcp_socket.cpp
    src/network/udp_listener.cpp
    src/updserver/updserver_client.cpp
    src/updserver/updserver_discovery.cpp
    src/xell/xell_client.cpp
    src/xell/xell_discovery.cpp
)

add_library(UpdClient::updclient_lib ALIAS updclient_lib)

target_include_directories(updclient_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(updclient_lib PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    tl::expected
)

if(WIN32 OR MINGW)
    target_link_libraries(updclient_lib PUBLIC ws2_32)
    add_compile_definitions(_WIN32_WINNT=0x0600)
endif()

if(BUILD_SHARED_LIBS)
    target_compile_definitions(updclient_lib PRIVATE UPDCLIENT_EXPORTS)
else()
    target_compile_definitions(updclient_lib PUBLIC UPDCLIENT_STATIC)
endif()

# CLI Executable target
if(UPDCLIENT_BUILD_CLI)
    add_executable(updclient src/main.cpp)
    target_link_libraries(updclient PRIVATE updclient_lib CLI11::CLI11)
endif()
