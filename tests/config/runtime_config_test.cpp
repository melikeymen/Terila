#include "config/runtime_config.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

    namespace fs = std::filesystem;

    class RuntimeConfigTest : public ::testing::Test {
    protected:
        void SetUp() override {
            std::error_code ec;
            directory_ = fs::temp_directory_path(ec) / "terila-runtime-config-test";
            fs::remove_all(directory_, ec);
        }

        void TearDown() override {
            std::error_code ec;
            fs::remove_all(directory_, ec);
        }

        fs::path directory_;
    };

    TEST_F(RuntimeConfigTest, CreatesTheDirectoryAndTheFile) {
        terila::config::RuntimeConfig defaults;
        fs::path written;
        ASSERT_TRUE(terila::config::ensure_runtime_config(directory_, defaults, &written));
        EXPECT_TRUE(fs::is_directory(directory_));
        EXPECT_EQ(written.string(),
            (directory_ / terila::config::kRuntimeConfigFile).string());
        EXPECT_TRUE(fs::exists(written));
    }

    TEST_F(RuntimeConfigTest, NeverRewritesAnExistingFile) {
        terila::config::RuntimeConfig defaults;
        ASSERT_TRUE(terila::config::ensure_runtime_config(directory_, defaults));

        const auto path = directory_ / terila::config::kRuntimeConfigFile;
        {
            std::ofstream out(path, std::ios::trunc);
            out << "[memory]\nmax-megabytes=512\n";
        }

        fs::path written;
        ASSERT_TRUE(terila::config::ensure_runtime_config(directory_, defaults, &written));
        EXPECT_TRUE(written.empty());

        terila::config::RuntimeConfig loaded;
        terila::config::load_runtime_config(loaded, path);
        EXPECT_EQ(loaded.max_memory_mb, 512u);
    }

    TEST_F(RuntimeConfigTest, ReadsSectionsAndKeys) {
        fs::create_directories(directory_);
        const auto path = directory_ / terila::config::kRuntimeConfigFile;
        {
            std::ofstream out(path);
            out << "# comment\n"
                   "[memory]\nmax-megabytes=4096\nmin-megabytes=512\n"
                   "[threads]\nmax=6\nmin=1\n";
        }

        terila::config::RuntimeConfig loaded;
        EXPECT_TRUE(terila::config::load_runtime_config(loaded, path));
        EXPECT_EQ(loaded.max_memory_mb, 4096u);
        EXPECT_EQ(loaded.min_memory_mb, 512u);
        EXPECT_EQ(loaded.max_threads, 6u);
        EXPECT_EQ(loaded.min_threads, 1u);
        EXPECT_EQ(loaded.usable_memory_mb(), 4096u - 512u);
    }

    TEST_F(RuntimeConfigTest, OutOfRangeValuesFallBack) {
        fs::create_directories(directory_);
        const auto path = directory_ / terila::config::kRuntimeConfigFile;
        {
            std::ofstream out(path);
            out << "[memory]\nmax-megabytes=1\nmin-megabytes=999999\n"
                   "[threads]\nmax=0\nmin=99\n";
        }

        terila::config::RuntimeConfig loaded;
        terila::config::load_runtime_config(loaded, path);
        EXPECT_GE(loaded.max_memory_mb, terila::config::detail::kMinMemoryMb);
        EXPECT_LT(loaded.min_memory_mb, loaded.max_memory_mb);
        EXPECT_GE(loaded.max_threads, 1u);
        EXPECT_LE(loaded.min_threads, loaded.max_threads);
    }

    TEST_F(RuntimeConfigTest, WorkerThreadsStayBetweenTheBounds) {
        terila::config::RuntimeConfig config;
        config.min_threads = 2;
        config.max_threads = 3;
        const auto count = config.worker_threads();
        EXPECT_GE(count, 2u);
        EXPECT_LE(count, 3u);
    }

    TEST_F(RuntimeConfigTest, CacheGrowsWithTheBudget) {
        terila::config::RuntimeConfig small;
        small.max_memory_mb = 512;
        small.min_memory_mb = 256;

        terila::config::RuntimeConfig large;
        large.max_memory_mb = 4096;
        large.min_memory_mb = 256;

        EXPECT_LE(terila::config::chunk_cache_capacity(small, 24),
            terila::config::chunk_cache_capacity(large, 24));
        EXPECT_GE(terila::config::chunk_cache_capacity(large, 24), 128u);
    }

    TEST_F(RuntimeConfigTest, MissingFileLeavesTheDefaults) {
        terila::config::RuntimeConfig loaded;
        loaded.max_memory_mb = 1234;
        EXPECT_FALSE(terila::config::load_runtime_config(loaded,
            directory_ / "does-not-exist.config"));
        EXPECT_EQ(loaded.max_memory_mb, 1234u);
    }

} // namespace
