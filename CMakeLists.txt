cmake_minimum_required(VERSION 3.20)
project(terila LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


add_executable(terila "src/main.cpp")

if(WIN32)
    target_compile_definitions(terila PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(MSVC)
    target_compile_options(terila PRIVATE /utf-8)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(terila PRIVATE -Wno-character-conversion)
endif()

target_include_directories(terila PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(terila PRIVATE
    "src/protocol/varint.hpp"
    "src/protocol/string.hpp"
    "src/protocol/packet.hpp"
    "src/protocol/handshake.hpp"
    "src/protocol/primitives.hpp"
    "src/protocol/status.hpp"
    "src/network/connection.hpp"
    "src/network/player_hub.hpp"
    "src/protocol/login.hpp"
    "src/data/registry_data.hpp"
    "src/data/registry_loader.hpp"
    "src/log/level.hpp"
    "src/log/logger.cpp"
    "src/log/logger.hpp"
    "src/protocol/play.hpp"
    "src/protocol/entity.hpp"
    "src/protocol/player_info.hpp"
    "src/protocol/block.hpp"
    "src/entity/mob.hpp"
    "src/world/block_world.hpp"
    "src/protocol/packet_ids.hpp"
    "src/protocol/packet_ids_loader.hpp"
    "src/protocol/uuid.hpp"
    "src/config/server_config.hpp"
    "src/world/chunk.hpp"
    "src/world/chunk_decoder.hpp"
    "src/world/chunk_encoder.hpp"
    "src/world/flat_generator.hpp"
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
target_link_libraries(terila_asio INTERFACE Threads::Threads)

# OpenSSL provides MD5 for offline-mode UUIDs. On Windows the vcpkg tree is the
# usual location, so point FindOpenSSL at it when no root was given explicitly.
if(WIN32 AND NOT DEFINED OPENSSL_ROOT_DIR)
    find_program(VCPKG_EXECUTABLE NAMES vcpkg)
    if(VCPKG_EXECUTABLE)
        get_filename_component(vcpkg_root "${VCPKG_EXECUTABLE}" DIRECTORY)
        if(EXISTS "${vcpkg_root}/installed/x64-windows/include/openssl/evp.h")
            set(OPENSSL_ROOT_DIR "${vcpkg_root}/installed/x64-windows")
            message(STATUS "OpenSSL root from vcpkg: ${OPENSSL_ROOT_DIR}")
        endif()
    endif()
endif()

find_package(OpenSSL REQUIRED)

target_link_libraries(terila PRIVATE nlohmann_json::nlohmann_json terila_asio OpenSSL::Crypto)



add_executable(protocol_tests
    "tests/protocol/varint_test.cpp"
    "tests/protocol/string_test.cpp"
    "tests/protocol/packet_test.cpp"
    "tests/protocol/handshake_test.cpp"
    "tests/protocol/primitives_test.cpp"
    "tests/protocol/status_test.cpp"
    "tests/protocol/login_test.cpp"
    "tests/protocol/uuid_test.cpp"
    "tests/protocol/entity_test.cpp"
    "tests/protocol/block_test.cpp"
    "tests/protocol/player_info_test.cpp"
    "src/log/logger.cpp"
)

if(MSVC)
    target_compile_options(protocol_tests PRIVATE /utf-8)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(protocol_tests PRIVATE -Wno-character-conversion)
endif()

target_include_directories(protocol_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${googletest_SOURCE_DIR}/googletest/include"
)
target_link_libraries(protocol_tests PRIVATE GTest::gtest_main nlohmann_json::nlohmann_json terila_asio OpenSSL::Crypto)

# Windows needs the OpenSSL runtime next to the executables.
if(WIN32)
    file(GLOB OPENSSL_RUNTIME_DLLS "${OPENSSL_ROOT_DIR}/bin/libcrypto-*.dll")
    if(OPENSSL_RUNTIME_DLLS)
        foreach(target terila protocol_tests)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${OPENSSL_RUNTIME_DLLS} $<TARGET_FILE_DIR:${target}>)
        endforeach()
    endif()
endif()

add_executable(chunk_tests
    "tests/world/chunk_decoder_test.cpp"
    "tests/world/chunk_encoder_test.cpp"
    "src/log/logger.cpp"
)

if(MSVC)
    target_compile_options(chunk_tests PRIVATE /utf-8)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(chunk_tests PRIVATE -Wno-character-conversion)
endif()

target_include_directories(chunk_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${googletest_SOURCE_DIR}/googletest/include"
)
target_link_libraries(chunk_tests PRIVATE GTest::gtest_main nlohmann_json::nlohmann_json terila_asio)

enable_testing()
add_test(NAME protocol_tests COMMAND protocol_tests)
add_test(NAME chunk_tests    COMMAND chunk_tests)