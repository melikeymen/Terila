cmake_minimum_required(VERSION 3.20)
project(terila LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


add_executable(terila "src/main.cpp")
target_compile_definitions(terila PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(terila PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(terila PRIVATE
    "src/protocol/varint.hpp"
    "src/protocol/string.hpp"
    "src/protocol/packet.hpp"
    "src/protocol/handshake.hpp"
    "src/protocol/primitives.hpp"
    "src/protocol/status.hpp"
    "src/network/connection.hpp"
    "src/protocol/login.hpp"
)

# Dependencies
include(FetchContent)

# Google Test
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# nlohmann/json
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# Standalone Asio
FetchContent_Declare(
  asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG        asio-1-30-2
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(asio)

add_library(terila_asio INTERFACE)
target_include_directories(terila_asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(terila_asio INTERFACE ASIO_STANDALONE)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
target_link_libraries(terila PRIVATE OpenSSL::Crypto)
target_link_libraries(terila_asio INTERFACE Threads::Threads)


target_link_libraries(terila PRIVATE nlohmann_json::nlohmann_json terila_asio)

# tests
add_executable(tests
    "tests/protocol/varint_test.cpp"
    "tests/protocol/string_test.cpp"
    "tests/protocol/packet_test.cpp"
    "tests/protocol/handshake_test.cpp"
    "tests/protocol/primitives_test.cpp"
    "tests/protocol/status_test.cpp"
    "tests/protocol/login_test.cpp"
)
target_include_directories(tests PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${googletest_SOURCE_DIR}/googletest/include"
)
target_link_libraries(tests PRIVATE GTest::gtest_main nlohmann_json::nlohmann_json terila_asio)