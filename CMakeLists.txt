cmake_minimum_required(VERSION 3.20)
project(mini_toolbridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# boost::json 需要编译库：sudo apt install -y libboost-json-dev
find_package(Boost 1.75 REQUIRED COMPONENTS json system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(tb_core STATIC
    src/toolbridge.cpp
    src/llm.cpp
    src/stream.cpp
    src/net_stream.cpp
    src/mcp_server.cpp
)
target_include_directories(tb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tb_core PUBLIC Boost::json Boost::system OpenSSL::SSL OpenSSL::Crypto Threads::Threads)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tb_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(tb apps/tb.cpp)
target_link_libraries(tb PRIVATE tb_core)

add_executable(stream_selftest tests/stream_selftest.cpp)
target_link_libraries(stream_selftest PRIVATE tb_core)

add_executable(validate_selftest tests/validate_selftest.cpp)
target_link_libraries(validate_selftest PRIVATE tb_core)

add_executable(mcp_selftest tests/mcp_selftest.cpp)
target_link_libraries(mcp_selftest PRIVATE tb_core)

enable_testing()
add_test(NAME stream_selftest COMMAND stream_selftest)
add_test(NAME mcp_selftest COMMAND mcp_selftest)
add_test(NAME validate_selftest COMMAND validate_selftest)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tb PRIVATE -Wall -Wextra -Wpedantic)
endif()
