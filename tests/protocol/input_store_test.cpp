#include "data/input_store.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace {

    // The tests build without TERILA_EMBEDDED_INPUT, so this exercises the directory side of
    // the store; the embedded table is checked by running an embedded build.
    class InputStoreTest : public ::testing::Test {
    protected:
        void SetUp() override {
            root_ = std::filesystem::temp_directory_path() / "terila_input_store_test";
            std::filesystem::remove_all(root_);
            std::filesystem::create_directories(root_ / "nested");
            std::ofstream(root_ / "nested" / "data.bin", std::ios::binary)
                << std::string("\x01\x02\x03", 3);
            terila::data::input::mount(root_.string());
        }

        void TearDown() override {
            terila::data::input::mount({});
            std::filesystem::remove_all(root_);
        }

        std::filesystem::path root_;
    };

}

TEST_F(InputStoreTest, reads_bytes_and_text) {
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(terila::data::input::read("nested/data.bin", bytes));
    ASSERT_EQ(bytes.size(), 3u);
    EXPECT_EQ(bytes[0], 1);
    EXPECT_EQ(bytes[2], 3);

    std::string text;
    ASSERT_TRUE(terila::data::input::read_text("nested/data.bin", text));
    EXPECT_EQ(text.size(), 3u);
}

TEST_F(InputStoreTest, reports_a_missing_file) {
    std::vector<std::uint8_t> bytes;
    EXPECT_FALSE(terila::data::input::read("nested/missing.bin", bytes));
}

TEST_F(InputStoreTest, lists_files_below_a_prefix) {
    const auto files = terila::data::input::files_under("nested");
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0], "nested/data.bin");

    EXPECT_TRUE(terila::data::input::files_under("absent").empty());
}

TEST_F(InputStoreTest, keeps_the_mounted_root) {
    EXPECT_FALSE(terila::data::input::is_embedded());
    EXPECT_EQ(terila::data::input::root(), root_.string());
    EXPECT_NE(terila::data::input::describe().find("disk"), std::string::npos);
}
