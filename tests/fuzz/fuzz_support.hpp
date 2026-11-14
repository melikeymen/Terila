#pragma once

#include "log/logger.hpp"

// Every fuzz target rejects almost every input on purpose, and each rejection logs a
// warning that would bury libFuzzer's own progress output. Only fatal messages survive.
#define TERILA_FUZZ_INITIALIZE()                                                 \
    extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {               \
        terila::log::Config cfg;                                                 \
        cfg.min_level = terila::log::Level::Fatal;                               \
        cfg.color_enabled = false;                                               \
        cfg.timestamp = terila::log::TimestampFormat::None;                      \
        cfg.exit_enabled = false;                                                \
        terila::log::init(cfg);                                                  \
        (void)argc;                                                              \
        (void)argv;                                                              \
        return 0;                                                                \
    }
