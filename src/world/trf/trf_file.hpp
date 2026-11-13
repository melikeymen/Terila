#pragma once

#include "world/trf/trf_error.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>

namespace terila::world::trf {

    // §19: every file operation in TRF goes through this class so the platform details
    // (sparse marking, ReplaceFileW, FlushFileBuffers) stay in one place.
    class TrfFile {
    public:
        TrfFile() = default;
        ~TrfFile();

        TrfFile(const TrfFile&) = delete;
        TrfFile& operator=(const TrfFile&) = delete;
        TrfFile(TrfFile&& other) noexcept;
        TrfFile& operator=(TrfFile&& other) noexcept;

        static std::expected<TrfFile, TrfError> open(const std::filesystem::path& path);

        // Creates the file at its final size. On Windows the sparse flag is applied before
        // the extension so NTFS does not allocate four gigabytes per region (§12.1 step 2).
        static std::expected<void, TrfError> create(const std::filesystem::path& path,
            std::uint64_t size);

        static std::expected<void, TrfError> replace(const std::filesystem::path& from,
            const std::filesystem::path& to);

        static std::expected<void, TrfError> remove(const std::filesystem::path& path);

        static std::expected<void, TrfError> sync_directory(const std::filesystem::path& directory);

        std::expected<void, TrfError> read_at(std::uint64_t offset, std::span<std::byte> out) const;
        std::expected<void, TrfError> write_at(std::uint64_t offset, std::span<const std::byte> data);
        std::expected<void, TrfError> resize(std::uint64_t size);
        std::expected<void, TrfError> flush_stream();
        std::expected<void, TrfError> barrier();
        std::expected<std::uint64_t, TrfError> size() const;

        bool valid() const noexcept { return file_ != nullptr; }
        void close() noexcept;

    private:
        std::FILE* file_ = nullptr;
    };

}
