cmake_minimum_required(VERSION 3.20)
project(mini_toolbridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# boost::json 需要编译库：sudo apt install -y libboost-json-dev
find_package(Boost 1.75 REQUIRED COMPONENTS json)

add_library(tb_core STATIC
    src/toolbridge.cpp
    src/llm.cpp
)
target_include_directories(tb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tb_core PUBLIC Boost::json)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tb_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(tb apps/tb.cpp)
target_link_libraries(tb PRIVATE tb_core)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tb PRIVATE -Wall -Wextra -Wpedantic)
endif()
