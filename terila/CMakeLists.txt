cmake_minimum_required(VERSION 3.20)
project(terila LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(terila "src/main.cpp")
target_include_directories(terila PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(varint_test "tests/protocol/varint_test.cpp")
target_include_directories(varint_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(varint_test PRIVATE GTest::gtest_main)