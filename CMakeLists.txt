cmake_minimum_required(VERSION 3.14)
project(reactor_kv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

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

add_executable(kv_server src/main.cpp)
target_link_libraries(kv_server PRIVATE kv_core)

add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE kv_core)

enable_testing()
add_test(NAME protocol_test COMMAND test_protocol)
