cmake_minimum_required(VERSION 3.20)
project(UpdClient VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(UPDCLIENT_IS_TOP_LEVEL ON)
else()
    set(UPDCLIENT_IS_TOP_LEVEL OFF)
endif()

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(UPDCLIENT_BUILD_CLI "Build the updclient command line tool" ON)
option(UPDCLIENT_BUILD_TESTS "Build the test suite" ${UPDCLIENT_IS_TOP_LEVEL})
option(UPDCLIENT_WARNINGS "Enable extra compiler warnings" ${UPDCLIENT_IS_TOP_LEVEL})
option(UPDCLIENT_INSTALL "Generate install and export rules" ${UPDCLIENT_IS_TOP_LEVEL})
option(UPDCLIENT_USE_STD_EXPECTED "Use std::expected instead of tl::expected (needs a C++23 standard library and CMAKE_CXX_STANDARD 23)" OFF)
option(UPDCLIENT_USE_SYSTEM_DEPS "Use installed dependencies found by find_package, fetching only the missing ones" OFF)

if(UPDCLIENT_IS_TOP_LEVEL)
    if(NOT DEFINED CMAKE_CXX_STANDARD)
        set(CMAKE_CXX_STANDARD 20)
    endif()
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

if(BUILD_SHARED_LIBS)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

include(GNUInstallDirs)
include(FetchContent)

# Dependencies. Each one comes from the first of:
#   1. a target that already exists (a parent project supplied it);
#   2. with UPDCLIENT_USE_SYSTEM_DEPS, an installed package (find_package CONFIG, at least the
#      version below, same major version);
#   3. FetchContent: the pinned tag below, or FETCHCONTENT_SOURCE_DIR_<NAME> if set. The names
#      cli11, spdlog, json and expected are fixed so those overrides keep working.
# Only the library's dependencies (spdlog, and tl::expected unless UPDCLIENT_USE_STD_EXPECTED) are
# looked at unless UPDCLIENT_BUILD_CLI is ON; CLI11 and nlohmann_json are not even declared then.
macro(updclient_dependency name package version target)
    if(TARGET ${target})
        message(STATUS "UpdClient: ${name}: using the existing ${target} target")
    else()
        if(UPDCLIENT_USE_SYSTEM_DEPS)
            find_package(${package} ${version} CONFIG QUIET)
        endif()
        if(TARGET ${target})
            message(STATUS "UpdClient: ${name}: using installed ${package} ${${package}_VERSION} (${${package}_DIR})")
        else()
            string(TOUPPER ${name} updclient_dependency_upper)
            if(FETCHCONTENT_FULLY_DISCONNECTED AND NOT FETCHCONTENT_SOURCE_DIR_${updclient_dependency_upper}
               AND NOT EXISTS "${FETCHCONTENT_BASE_DIR}/${name}-src")
                message(FATAL_ERROR "UpdClient: ${name} is needed, FETCHCONTENT_FULLY_DISCONNECTED is ON and "
                    "nothing provides it. Pass -DFETCHCONTENT_SOURCE_DIR_${updclient_dependency_upper}=<source tree>, "
                    "or install ${package} and configure with -DUPDCLIENT_USE_SYSTEM_DEPS=ON.")
            endif()
            if(FETCHCONTENT_SOURCE_DIR_${updclient_dependency_upper})
                message(STATUS "UpdClient: ${name}: building from ${FETCHCONTENT_SOURCE_DIR_${updclient_dependency_upper}}")
            else()
                message(STATUS "UpdClient: ${name}: fetching with FetchContent")
            endif()
            FetchContent_MakeAvailable(${name})
        endif()
    endif()
endmacro()

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
)
set(SPDLOG_INSTALL ${UPDCLIENT_INSTALL} CACHE BOOL "")
updclient_dependency(spdlog spdlog 1.14 spdlog::spdlog)

# spdlog 1.14's bundled fmt 10.2 does not compile its consteval format-string checks
# under recent Clang releases; fall back to fmt's non-consteval path there.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    foreach(updclient_spdlog_target spdlog spdlog_header_only)
        if(TARGET ${updclient_spdlog_target})
            get_target_property(updclient_spdlog_imported ${updclient_spdlog_target} IMPORTED)
            get_target_property(updclient_spdlog_type ${updclient_spdlog_target} TYPE)
            if(NOT updclient_spdlog_imported)
                if(updclient_spdlog_type STREQUAL "INTERFACE_LIBRARY")
                    target_compile_definitions(${updclient_spdlog_target} INTERFACE FMT_CONSTEVAL=)
                else()
                    target_compile_definitions(${updclient_spdlog_target} PUBLIC FMT_CONSTEVAL=)
                endif()
            endif()
        endif()
    endforeach()
endif()

if(UPDCLIENT_USE_STD_EXPECTED)
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("
#include <version>
#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202211L
#error no std::expected
#endif
#include <expected>
int main() { std::expected<int, int> e(1); return *e - 1; }
" UPDCLIENT_HAVE_STD_EXPECTED)
    if(NOT UPDCLIENT_HAVE_STD_EXPECTED)
        message(FATAL_ERROR "UPDCLIENT_USE_STD_EXPECTED is ON but std::expected is not available with the current compiler and CMAKE_CXX_STANDARD")
    endif()
else()
    FetchContent_Declare(
        expected
        GIT_REPOSITORY https://github.com/TartanLlama/expected.git
        GIT_TAG        v1.1.0
    )
    set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(EXPECTED_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
    # tl-expected 1.1.0 still reports version 1.0.0.
    updclient_dependency(expected tl-expected 1.0 tl::expected)
endif()

if(UPDCLIENT_BUILD_CLI)
    FetchContent_Declare(
        cli11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
        GIT_TAG        v2.4.2
    )
    FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
    )
    updclient_dependency(cli11 CLI11 2.4 CLI11::CLI11)
    updclient_dependency(json nlohmann_json 3.11 nlohmann_json::nlohmann_json)
endif()

function(updclient_apply_warnings target)
    if(UPDCLIENT_WARNINGS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endif()
endfunction()

# Library: every .cpp under src/ except the CLI. Adding a file needs no CMake edit.
file(GLOB_RECURSE UPDCLIENT_LIB_SOURCES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} src/*.cpp)
list(FILTER UPDCLIENT_LIB_SOURCES EXCLUDE REGEX "^src/(main\\.cpp$|cli/)")
file(GLOB_RECURSE UPDCLIENT_CLI_SOURCES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} src/cli/*.cpp)

add_library(updclient_lib ${UPDCLIENT_LIB_SOURCES})
add_library(UpdClient::updclient_lib ALIAS updclient_lib)

target_compile_features(updclient_lib PUBLIC cxx_std_20)
set_target_properties(updclient_lib PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(updclient_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(updclient_lib PRIVATE spdlog::spdlog)

# Part of the library's ABI: consumers see the same definition as the library.
if(UPDCLIENT_USE_STD_EXPECTED)
    target_compile_definitions(updclient_lib PUBLIC UPDCLIENT_USE_STD_EXPECTED)
else()
    target_link_libraries(updclient_lib PUBLIC tl::expected)
endif()

if(WIN32)
    target_compile_definitions(updclient_lib PRIVATE _WIN32_WINNT=0x0600 WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(updclient_lib PRIVATE ws2_32)
endif()

if(BUILD_SHARED_LIBS)
    target_compile_definitions(updclient_lib PRIVATE UPDCLIENT_EXPORTS)
else()
    target_compile_definitions(updclient_lib PUBLIC UPDCLIENT_STATIC)
endif()

updclient_apply_warnings(updclient_lib)

# CLI: the only target that depends on CLI11 and nlohmann_json.
if(UPDCLIENT_BUILD_CLI)
    add_executable(updclient src/main.cpp ${UPDCLIENT_CLI_SOURCES})
    set_target_properties(updclient PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(updclient PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(updclient PRIVATE
        UpdClient::updclient_lib
        CLI11::CLI11
        nlohmann_json::nlohmann_json
        spdlog::spdlog
    )
    updclient_apply_warnings(updclient)
endif()

if(UPDCLIENT_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    enable_testing()
    add_subdirectory(tests)
endif()

if(UPDCLIENT_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS updclient_lib
        EXPORT UpdClientTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(DIRECTORY include/updclient DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    if(UPDCLIENT_BUILD_CLI)
        install(TARGETS updclient RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    set(UPDCLIENT_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/UpdClient)
    install(EXPORT UpdClientTargets
        NAMESPACE UpdClient::
        DESTINATION ${UPDCLIENT_CMAKE_DIR}
    )

    if(BUILD_SHARED_LIBS)
        set(UPDCLIENT_CONFIG_SHARED TRUE)
    else()
        set(UPDCLIENT_CONFIG_SHARED FALSE)
    endif()
    if(UPDCLIENT_USE_STD_EXPECTED)
        set(UPDCLIENT_CONFIG_STD_EXPECTED TRUE)
    else()
        set(UPDCLIENT_CONFIG_STD_EXPECTED FALSE)
    endif()
    configure_package_config_file(
        cmake/UpdClientConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/UpdClientConfig.cmake
        INSTALL_DESTINATION ${UPDCLIENT_CMAKE_DIR}
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/UpdClientConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/UpdClientConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/UpdClientConfigVersion.cmake
        DESTINATION ${UPDCLIENT_CMAKE_DIR}
    )
endif()
