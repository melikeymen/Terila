cmake_minimum_required(VERSION 3.20)
project(terila LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# Minecraft protocol this build speaks. The buildable targets are listed in
# src/version/mc_version.hpp; this picks one of them and refuses anything older than 26.3
# there, at compile time.
set(TERILA_MC_TARGET "26.3" CACHE STRING "Minecraft protocol target, for example 26.3")

# "26.3" -> 2603, the same value TERILA_MC_CODE produces in src/version/mc_version.hpp.
string(REPLACE "." ";" TERILA_MC_TARGET_PARTS "${TERILA_MC_TARGET}")
list(LENGTH TERILA_MC_TARGET_PARTS TERILA_MC_TARGET_PART_COUNT)
if(NOT TERILA_MC_TARGET_PART_COUNT EQUAL 2)
    message(FATAL_ERROR "TERILA_MC_TARGET must look like 26.3, got '${TERILA_MC_TARGET}'")
endif()
list(GET TERILA_MC_TARGET_PARTS 0 TERILA_MC_TARGET_MAJOR)
list(GET TERILA_MC_TARGET_PARTS 1 TERILA_MC_TARGET_MINOR)
math(EXPR TERILA_MC_TARGET_CODE
    "${TERILA_MC_TARGET_MAJOR} * 100 + ${TERILA_MC_TARGET_MINOR}")

# Directory scope, so the server, the tests and the fuzzers all see the same target.
add_compile_definitions(TERILA_MC_TARGET=${TERILA_MC_TARGET_CODE})

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
    "src/version/mc_version.hpp"
    "src/version/features.hpp"
    "src/protocol/varint.hpp"
    "src/protocol/string.hpp"
    "src/protocol/packet.hpp"
    "src/protocol/handshake.hpp"
    "src/protocol/primitives.hpp"
    "src/protocol/status.hpp"
    "src/entity/item_drop.hpp"
    "src/protocol/item.hpp"
    "src/network/connection.hpp"
    "src/network/connection.cpp"
    "src/network/connection_handshake.cpp"
    "src/network/connection_login.cpp"
    "src/network/connection_play.cpp"
    "src/network/connection_health.cpp"
    "src/network/connection_chunks.cpp"
    "src/network/connection_world.cpp"
    "src/network/connection_inventory.cpp"
    "src/network/connection_menu.cpp"
    "src/protocol/menu.hpp"
    "src/protocol/chat.hpp"
    "src/async/task.hpp"
    "src/nbt/nbt.hpp"
    "src/nbt/nbt_value.hpp"
    "src/world/block_cache.hpp"
    "src/world/chunk_cache.hpp"
    "src/world/chunk_jobs.cpp"
    "src/world/chunk_jobs.hpp"
    "src/world/world_context.hpp"
    "src/nbt/nbt_budget.hpp"
    "src/nbt/nbt_codec.hpp"
    "src/item/components.hpp"
    "src/item/components.cpp"
    "src/item/item_parser.hpp"
    "src/item/item_parser.cpp"
    "src/nbt/nbt_text.hpp"
    "src/commands/text.hpp"
    "src/commands/selector.hpp"
    "src/commands/diagnostic.hpp"
    "src/commands/cursor.hpp"
    "src/commands/value.hpp"
    "src/commands/parser.hpp"
    "src/commands/origin.hpp"
    "src/commands/node.hpp"
    "src/commands/registry.hpp"
    "src/commands/registry.cpp"
    "src/commands/command_time.cpp"
    "src/commands/command_give.cpp"
    "src/commands/command_execute.cpp"
    "src/commands/command_gamemode.cpp"
    "src/commands/command_teleport.cpp"
    "src/commands/command_summon.cpp"
    "src/commands/command_message.cpp"
    "src/network/connection_commands.cpp"
    "src/data/recipe_book.hpp"
    "src/data/recipe_book.cpp"
    "src/network/connection_entities.cpp"
    "src/network/player_hub.hpp"
    "src/network/mojang_profile.hpp"
    "src/network/mojang_profile.cpp"
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
    "src/world/falling_blocks.hpp"
    "src/world/fluid_flow.hpp"
    "src/world/block_break.hpp"
    "src/world/light_engine.hpp"
    "src/world/trf/trf_error.hpp"
    "src/world/trf/trf_format.hpp"
    "src/world/trf/trf_crc32c.hpp"
    "src/world/trf/trf_crc32c.cpp"
    "src/world/trf/trf_sha256.hpp"
    "src/world/trf/trf_sha256.cpp"
    "src/world/trf/trf_file.hpp"
    "src/world/trf/trf_file.cpp"
    "src/world/trf/trf_header.hpp"
    "src/world/trf/trf_header.cpp"
    "src/world/trf/trf_slot.hpp"
    "src/world/trf/trf_slot.cpp"
    "src/world/trf/trf_lz4.hpp"
    "src/world/trf/trf_lz4.cpp"
    "src/world/trf/trf_region.hpp"
    "src/world/trf/trf_region.cpp"
    "src/world/trf/trf_store.hpp"
    "src/world/trf/trf_store.cpp"
    "src/protocol/packet_ids.hpp"
    "src/protocol/packet_ids_loader.hpp"
    "src/data/input_store.hpp"
    "src/data/input_store.cpp"
    "src/protocol/uuid.hpp"
    "src/config/server_config.hpp"
    "src/world/chunk.hpp"
    "src/world/chunk_decoder.hpp"
    "src/world/chunk_encoder.hpp"
    "src/world/flat_generator.hpp"
    "src/world/terrain_biomes.hpp"
    "src/world/terrain_generator.hpp"
    "src/world/terrain_noise.hpp"
    "src/world/worldgen/chunk_sections.hpp"
    "src/world/worldgen/column.hpp"
    "src/world/worldgen/density/climate.hpp"
    "src/world/worldgen/density/density.hpp"
    "src/world/worldgen/density/noise.hpp"
    "src/world/worldgen/density_grid.hpp"
    "src/world/worldgen/generator.hpp"
    "src/world/worldgen/settings.hpp"
    "src/world/worldgen/surface/material.hpp"
    "src/world/worldgen/surface/ores.hpp"
    "src/world/worldgen/surface/vegetation.hpp"
    "src/world/world_generator.hpp"
)

# Compiling the data into the binary. The server reads its files through
# src/data/input_store.cpp, so with this on it starts without an input/ directory at all;
# with it off (the default) the same code reads the directory at run time.
option(TERILA_EMBED_INPUT "Compile the data files the server reads into the binary" OFF)
set(TERILA_INPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/input" CACHE PATH
    "Data directory read at run time, or embedded when TERILA_EMBED_INPUT is on")

if(TERILA_EMBED_INPUT)
    # Exactly the files the server loads at startup. A new read through input_store belongs
    # here too, otherwise the embedded build reports it as not compiled in.
    set(TERILA_EMBED_FILES
        "packets/ids.json"
        "registries/index.json"
        "registries/dump/minecraft/data_component_type.json"
        "registries/dump/minecraft/command_argument_type.json"
        "registries/dump/minecraft/enchantment.json"
        "tags/client_tags.bin"
        "tags/items.json"
        "tags/blocks.json"
        "recipes/minecraft.nbt"
        "recipes/minecraft.json"
        "loot/minecraft.nbt"
        "advancements/minecraft.nbt"
        "commands/tree.nbt"
        "blocks/state_ids.json"
        "blocks/minecraft.json"
        "worldgen/biomes.json"
        "worldgen/biome_parameters.json"
        "worldgen/density_functions.json"
        "worldgen/noise_settings.json"
        "worldgen/dimensions.json"
        "registries/dump/minecraft/worldgen/material_condition.json"
        "registries/dump/minecraft/worldgen/material_rule.json"
        "registries/dump/minecraft/worldgen/noise.json"
        "entities/minecraft.json"
        "items/minecraft.json"
    )
    file(GLOB_RECURSE TERILA_EMBED_REGISTRY_PAYLOADS CONFIGURE_DEPENDS
        RELATIVE "${TERILA_INPUT_DIR}" "${TERILA_INPUT_DIR}/registries/nbt/*.nbt")
    list(APPEND TERILA_EMBED_FILES ${TERILA_EMBED_REGISTRY_PAYLOADS})
    list(SORT TERILA_EMBED_FILES)

    set(TERILA_EMBED_DIR "${CMAKE_CURRENT_BINARY_DIR}/embedded")
    file(MAKE_DIRECTORY "${TERILA_EMBED_DIR}")

    # A chunk is 512 KiB of raw data, which becomes a few megabytes of C++ at most: no single
    # translation unit should carry a whole multi-megabyte data file.
    set(TERILA_EMBED_CHUNK_BYTES 524288)
    set(terila_embed_manifest "")
    string(APPEND terila_embed_manifest "input_dir ${TERILA_INPUT_DIR}\n")
    string(APPEND terila_embed_manifest "chunk_bytes ${TERILA_EMBED_CHUNK_BYTES}\n")
    set(terila_embed_deps "")
    set(terila_embed_chunks 0)
    set(terila_embed_index 0)

    foreach(relative IN LISTS TERILA_EMBED_FILES)
        set(full "${TERILA_INPUT_DIR}/${relative}")
        if(NOT EXISTS "${full}")
            message(FATAL_ERROR
                "TERILA_EMBED_INPUT needs ${relative}, which is missing from ${TERILA_INPUT_DIR}")
        endif()

        file(SIZE "${full}" size)
        if(size EQUAL 0)
            message(FATAL_ERROR "TERILA_EMBED_INPUT cannot embed the empty file ${relative}")
        endif()

        math(EXPR chunk_count "((${size} - 1) / ${TERILA_EMBED_CHUNK_BYTES}) + 1")
        string(APPEND terila_embed_manifest
            "entry ${terila_embed_index} ${size} ${terila_embed_chunks} ${chunk_count} ${relative}\n")

        set(offset 0)
        foreach(unused RANGE 1 ${chunk_count})
            math(EXPR remaining "${size} - ${offset}")
            set(length ${TERILA_EMBED_CHUNK_BYTES})
            if(remaining LESS length)
                set(length ${remaining})
            endif()
            string(APPEND terila_embed_manifest
                "chunk ${terila_embed_chunks} ${offset} ${length} ${relative}\n")
            math(EXPR offset "${offset} + ${length}")
            math(EXPR terila_embed_chunks "${terila_embed_chunks} + 1")
        endforeach()

        math(EXPR terila_embed_index "${terila_embed_index} + 1")
        list(APPEND terila_embed_deps "${full}")
    endforeach()

    set(TERILA_EMBED_MANIFEST "${TERILA_EMBED_DIR}/manifest.txt")
    file(WRITE "${TERILA_EMBED_MANIFEST}" "${terila_embed_manifest}")

    set(TERILA_EMBED_SOURCES
        "${TERILA_EMBED_DIR}/embedded_input.hpp"
        "${TERILA_EMBED_DIR}/embedded_input_table.cpp")
    math(EXPR terila_embed_last_chunk "${terila_embed_chunks} - 1")
    foreach(index RANGE 0 ${terila_embed_last_chunk})
        list(APPEND TERILA_EMBED_SOURCES "${TERILA_EMBED_DIR}/embedded_${index}.cpp")
    endforeach()

    add_executable(terila_embed_input "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_input.cpp")
    if(MSVC)
        target_compile_options(terila_embed_input PRIVATE /utf-8)
    endif()

    add_custom_command(
        OUTPUT ${TERILA_EMBED_SOURCES}
        COMMAND terila_embed_input "${TERILA_EMBED_MANIFEST}" "${TERILA_EMBED_DIR}"
        DEPENDS terila_embed_input "${TERILA_EMBED_MANIFEST}" ${terila_embed_deps}
        COMMENT "Embedding ${terila_embed_index} data files into the binary"
        VERBATIM)

    target_sources(terila PRIVATE ${TERILA_EMBED_SOURCES})
    target_include_directories(terila PRIVATE "${TERILA_EMBED_DIR}")
    target_compile_definitions(terila PRIVATE TERILA_EMBEDDED_INPUT=1)
    message(STATUS
        "TERILA_EMBED_INPUT: ${terila_embed_index} files, ${terila_embed_chunks} chunks")
else()
    message(STATUS "TERILA_EMBED_INPUT: off, the server reads ${TERILA_INPUT_DIR} at run time")
endif()

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

# simdjson parses the block registry dump
FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG        v3.12.0
  GIT_SHALLOW    TRUE
)
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
set(SIMDJSON_JUST_LIBRARY ON CACHE BOOL "" FORCE)
set(SIMDJSON_BUILD_STATIC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(simdjson)

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

# A Windows executable only starts next to its runtime DLLs: OpenSSL comes from vcpkg and
# the AddressSanitizer runtime from the MSVC toolset, and neither is on PATH when the
# binary is started by hand or by a debugger outside Visual Studio.
set(TERILA_OPENSSL_DLLS "")
if(WIN32)
    file(GLOB TERILA_OPENSSL_DLLS "${OPENSSL_ROOT_DIR}/bin/libcrypto-*.dll")
endif()

function(terila_copy_runtime target)
    if(NOT WIN32)
        return()
    endif()

    if(TERILA_OPENSSL_DLLS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${TERILA_OPENSSL_DLLS} $<TARGET_FILE_DIR:${target}>)
    endif()

    # /fsanitize=address and /fsanitize=fuzzer link the ASan runtime dynamically.
    if(MSVC)
        get_filename_component(terila_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        file(GLOB TERILA_ASAN_DLLS "${terila_msvc_bin}/clang_rt.asan_dynamic-*.dll")
        if(TERILA_ASAN_DLLS)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${TERILA_ASAN_DLLS} $<TARGET_FILE_DIR:${target}>)
        endif()
    endif()
endfunction()

target_link_libraries(terila PRIVATE nlohmann_json::nlohmann_json simdjson::simdjson terila_asio OpenSSL::Crypto OpenSSL::SSL)
if (WIN32)
    # The account lookup opens plain TLS sockets of its own.
    target_link_libraries(terila PRIVATE ws2_32)
endif()



add_executable(protocol_tests
    "tests/protocol/inbound_play_test.cpp"
    "tests/protocol/version_test.cpp"
    "tests/protocol/varint_test.cpp"
    "tests/protocol/string_test.cpp"
    "tests/protocol/packet_test.cpp"
    "tests/protocol/handshake_test.cpp"
    "tests/protocol/primitives_test.cpp"
    "tests/protocol/status_test.cpp"
    "tests/protocol/login_test.cpp"
    "tests/protocol/uuid_test.cpp"
    "tests/protocol/components_test.cpp"
    "tests/protocol/chat_test.cpp"
    "src/item/components.cpp"
    "tests/protocol/entity_test.cpp"
    "tests/protocol/player_state_test.cpp"
    "tests/protocol/block_test.cpp"
    "tests/protocol/player_info_test.cpp"
    "tests/async/task_test.cpp"
    "tests/protocol/nbt_reader_test.cpp"
    "tests/protocol/player_hub_test.cpp"
    "tests/protocol/command_tree_test.cpp"
    "tests/commands/selector_test.cpp"
    "tests/commands/dispatch_test.cpp"
    "tests/protocol/snbt_test.cpp"
    "tests/protocol/item_parser_test.cpp"
    "tests/protocol/input_store_test.cpp"
    "src/item/components.cpp"
    "src/item/item_parser.cpp"
    "src/commands/registry.cpp"
    "src/commands/command_time.cpp"
    "src/commands/command_give.cpp"
    "src/commands/command_execute.cpp"
    "src/commands/command_gamemode.cpp"
    "src/commands/command_teleport.cpp"
    "src/commands/command_summon.cpp"
    "src/commands/command_message.cpp"
    "src/data/input_store.cpp"
    "src/log/logger.cpp"
)

add_executable(nbt_tests "tests/nbt/nbt_tests.cpp"
    "src/log/logger.cpp")
if(MSVC)
    target_compile_options(nbt_tests PRIVATE /utf-8)
endif()
target_include_directories(nbt_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${googletest_SOURCE_DIR}/googletest/include"
)
target_link_libraries(nbt_tests PRIVATE GTest::gtest_main)

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

terila_copy_runtime(terila)
terila_copy_runtime(protocol_tests)
terila_copy_runtime(nbt_tests)

add_executable(chunk_tests
    "src/world/chunk_jobs.cpp"
    "src/data/input_store.cpp"
    "tests/world/block_break_test.cpp"
    "tests/world/falling_blocks_test.cpp"
    "tests/world/fluid_flow_test.cpp"
    "tests/world/chunk_cache_test.cpp"
    "tests/world/chunk_decoder_test.cpp"
    "tests/world/terrain_generator_test.cpp"
    "tests/world/chunk_encoder_test.cpp"
    "tests/world/light_engine_test.cpp"
    "tests/world/mob_test.cpp"
    "tests/world/relight_test.cpp"
    "src/log/logger.cpp"
)

if(MSVC)
    target_compile_options(chunk_tests PRIVATE /utf-8)
endif()

if(WIN32)
    # Tools and tests share headers with the server, which relies on these.
    target_compile_definitions(chunk_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_definitions(protocol_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(chunk_tests PRIVATE -Wno-character-conversion)
endif()

target_include_directories(chunk_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${googletest_SOURCE_DIR}/googletest/include"
)
target_link_libraries(chunk_tests PRIVATE GTest::gtest_main nlohmann_json::nlohmann_json terila_asio)

include(cmake/parity_dump.cmake)

add_executable(trf_tests
    "tests/world/trf_tests.cpp"
    "tests/world/fuzz_smoke_test.cpp"
    "tests/fuzz/regression_tests.cpp"
    "src/world/trf/trf_crc32c.cpp"
    "src/world/trf/trf_sha256.cpp"
    "src/world/trf/trf_file.cpp"
    "src/world/trf/trf_header.cpp"
    "src/world/trf/trf_slot.cpp"
    "src/world/trf/trf_lz4.cpp"
    "src/world/trf/trf_region.cpp"
    "src/world/trf/trf_store.cpp"
    "src/log/logger.cpp"
)

if(MSVC)
    target_compile_options(trf_tests PRIVATE /utf-8)
endif()

if(WIN32)
    target_compile_definitions(trf_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(trf_tests PRIVATE -Wno-character-conversion)
endif()

target_include_directories(trf_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${googletest_SOURCE_DIR}/googletest/include"
)
# The saved fuzz inputs are replayed from tests/fuzz/regressions.
target_compile_definitions(trf_tests PRIVATE
    TERILA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
# The portable smoke test also probes the protocol readers, which build JSON replies.
target_link_libraries(trf_tests PRIVATE GTest::gtest_main OpenSSL::Crypto
    nlohmann_json::nlohmann_json)
terila_copy_runtime(trf_tests)

# libFuzzer targets, one executable per byte-facing parser so their corpora stay
# separate. MSVC 2022 17.4+ and Clang both support this, and the sanitizer comes from
# TERILA_FUZZ_SANITIZER so the preset name can advertise ASan, TSan or UBSan.
option(TERILA_BUILD_FUZZERS "Build libFuzzer targets" OFF)
set(TERILA_FUZZ_SANITIZER "address" CACHE STRING
    "Sanitizer for the fuzz targets: address, thread, undefined or none")

function(terila_add_fuzzer name)
    add_executable(${name} ${ARGN} "src/log/logger.cpp")
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    if(WIN32)
        target_compile_definitions(${name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    # The protocol readers build their JSON replies with nlohmann, exactly like the server.
    target_link_libraries(${name} PRIVATE nlohmann_json::nlohmann_json)

    if(MSVC)
        if(TERILA_FUZZ_SANITIZER STREQUAL "thread" OR
            TERILA_FUZZ_SANITIZER STREQUAL "undefined")
            message(FATAL_ERROR
                "cl.exe cannot combine /fsanitize=fuzzer with ${TERILA_FUZZ_SANITIZER}. "
                "Configure this preset with -DCMAKE_CXX_COMPILER=clang-cl.exe instead.")
        endif()
        target_compile_options(${name} PRIVATE /utf-8 /Od /Z7 /fsanitize=fuzzer)
        target_link_options(${name} PRIVATE /fsanitize=fuzzer)
        if(TERILA_FUZZ_SANITIZER STREQUAL "address")
            target_compile_options(${name} PRIVATE /fsanitize=address)
            target_link_options(${name} PRIVATE /fsanitize=address)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(TERILA_FUZZ_SANITIZER STREQUAL "none")
            target_compile_options(${name} PRIVATE -fsanitize=fuzzer -g -O1)
            target_link_options(${name} PRIVATE -fsanitize=fuzzer)
        else()
            target_compile_options(${name} PRIVATE
                -fsanitize=fuzzer,${TERILA_FUZZ_SANITIZER} -g -O1)
            target_link_options(${name} PRIVATE
                -fsanitize=fuzzer,${TERILA_FUZZ_SANITIZER})
        endif()
    endif()

    terila_copy_runtime(${name})
endfunction()

if(TERILA_BUILD_FUZZERS)
    terila_add_fuzzer(fuzz_nbt "tests/fuzz/fuzz_nbt.cpp")
    terila_add_fuzzer(fuzz_trf_decode
        "tests/fuzz/fuzz_trf_decode.cpp"
        "src/world/trf/trf_crc32c.cpp"
        "src/world/trf/trf_sha256.cpp"
        "src/world/trf/trf_file.cpp"
        "src/world/trf/trf_header.cpp"
        "src/world/trf/trf_slot.cpp"
    )
    target_link_libraries(fuzz_trf_decode PRIVATE OpenSSL::Crypto)

    terila_add_fuzzer(fuzz_protocol "tests/fuzz/fuzz_protocol.cpp")
    terila_add_fuzzer(fuzz_chunk "tests/fuzz/fuzz_chunk.cpp")
endif()

enable_testing()
add_test(NAME protocol_tests COMMAND protocol_tests)
add_test(NAME nbt_tests COMMAND nbt_tests)
add_test(NAME chunk_tests    COMMAND chunk_tests)
add_test(NAME trf_tests      COMMAND trf_tests)
