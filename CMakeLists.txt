cmake_minimum_required(VERSION 3.14)
project(reactor_kv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Debug, Release, RelWithDebInfo" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

option(KV_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(KV_SOURCES
    src/epoller.cpp
    src/channel.cpp
    src/event_loop.cpp
    src/buffer.cpp
    src/tcp_server.cpp
    src/tcp_connection.cpp
    src/protocol.cpp
    src/kv_store.cpp
    src/thread_pool.cpp
    src/async_logger.cpp
)

add_library(kv_core STATIC ${KV_SOURCES})
target_include_directories(kv_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(kv_core PUBLIC pthread)

if(KV_SANITIZE)
    target_compile_options(kv_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(kv_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(kv_server src/main.cpp)
target_link_libraries(kv_server PRIVATE kv_core)

add_executable(kv_bench src/bench_client.cpp)

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
FetchContent_MakeAvailable(googletest)

add_executable(kv_tests
    tests/test_main.cpp
    tests/test_protocol.cpp
    tests/test_buffer.cpp
    tests/test_kv_store.cpp
    tests/test_event_loop.cpp
    tests/test_thread_pool.cpp
    tests/test_integration.cpp
)
target_link_libraries(kv_tests PRIVATE kv_core GTest::gtest)

include(CTest)
enable_testing()
add_test(NAME kv_tests COMMAND kv_tests)
add_test(NAME graceful_shutdown
         COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/test_graceful.sh $<TARGET_FILE:kv_server>)
