cmake_minimum_required(VERSION 3.20)
project(terila LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


add_executable(terila "src/main.cpp")
target_include_directories(terila PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(terila PRIVATE
    "src/protocol/varint.hpp"
    "src/protocol/string.hpp"
    "src/protocol/packet.hpp"
)

# Google Test
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# tests
add_executable(tests
    "tests/protocol/varint_test.cpp"
    "tests/protocol/string_test.cpp"
    "tests/protocol/packet_test.cpp"
)
target_include_directories(tests PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${googletest_SOURCE_DIR}/googletest/include"
)
target_link_libraries(tests PRIVATE GTest::gtest_main)