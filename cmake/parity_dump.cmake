# The block and biome ids of one generated chunk, in the text format the parity tools read.
#
# This target exists for tools/parity: it prints what the generator produced so a region file
# written by the real server can be diffed against it block by block.

add_executable(worldgen_dump
    "tools/worldgen_dump.cpp"
    "src/data/input_store.cpp"
    "src/log/logger.cpp"
)

target_include_directories(worldgen_dump PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(worldgen_dump PRIVATE nlohmann_json::nlohmann_json simdjson::simdjson)

if(MSVC)
    target_compile_options(worldgen_dump PRIVATE /utf-8)
endif()

if(WIN32)
    target_compile_definitions(worldgen_dump PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

terila_copy_runtime(worldgen_dump)
