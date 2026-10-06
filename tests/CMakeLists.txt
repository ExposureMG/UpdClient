set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

file(GLOB_RECURSE UPDCLIENT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(updclient_tests ${UPDCLIENT_TEST_SOURCES})
set_target_properties(updclient_tests PROPERTIES CXX_EXTENSIONS OFF)
target_compile_features(updclient_tests PRIVATE cxx_std_20)
target_include_directories(updclient_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(updclient_tests PRIVATE UpdClient::updclient_lib Threads::Threads)

if(WIN32)
    target_compile_definitions(updclient_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(updclient_tests PRIVATE ws2_32)
endif()

if(COMMAND updclient_apply_warnings)
    updclient_apply_warnings(updclient_tests)
endif()

# One ctest entry runs the whole binary; run it directly with --list, --filter <text> or --verbose
# for finer control.
add_test(NAME updclient_tests COMMAND updclient_tests)
set_tests_properties(updclient_tests PROPERTIES TIMEOUT 120)
