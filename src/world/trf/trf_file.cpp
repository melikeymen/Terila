#include "world/trf/trf_file.hpp"

#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#endif

namespace terila::world::trf {

    namespace {

        std::string system_error_detail(const char* what) {
            return std::string(what) + ": " + std::strerror(errno);
        }

#ifdef _WIN32
        std::wstring wide_path(const std::filesystem::path& path) {
            std::wstring text = path.wstring();
            if (text.size() >= 240 && text.rfind(L"\\\\?\\", 0) != 0) {
                text = L"\\\\?\\" + std::filesystem::absolute(path).wstring();
            }
            return text;
        }
#endif

        // Sets the logical size of a region. On Windows _chsize_s grows a file by writing
        // zeroes through it, which allocates every block of a sparse region; SetEndOfFile
        // only moves the end of the file, so the unwritten tail stays unallocated and a
        // region costs what it actually stores.
        std::expected<void, TrfError> set_file_size(std::FILE* handle, std::uint64_t size) {
#ifdef _WIN32
            const intptr_t raw = _get_osfhandle(_fileno(handle));
            if (raw == -1) {
                return std::unexpected(make_error(TrfErrorCode::Io,
                    "cannot size region: no file handle"));
            }
            HANDLE device = reinterpret_cast<HANDLE>(raw);
            LARGE_INTEGER target;
            target.QuadPart = static_cast<LONGLONG>(size);
            if (SetFilePointerEx(device, target, nullptr, FILE_BEGIN) == 0 ||
                SetEndOfFile(device) == 0) {
                return std::unexpected(make_error(TrfErrorCode::Io,
                    "cannot size region (windows error " +
                    std::to_string(GetLastError()) + ")"));
            }
            return {};
#else
            if (ftruncate(fileno(handle), static_cast<off_t>(size)) != 0) {
                return std::unexpected(make_error(TrfErrorCode::Io,
                    system_error_detail("cannot size region")));
            }
            return {};
#endif
        }

        std::FILE* open_handle(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
            std::wstring wide_mode;
            for (const char* c = mode; *c; ++c) {
                wide_mode.push_back(static_cast<wchar_t>(*c));
            }
            return _wfopen(wide_path(path).c_str(), wide_mode.c_str());
#else
            return std::fopen(path.c_str(), mode);
#endif
        }

    }

    TrfFile::~TrfFile() {
        close();
    }

    TrfFile::TrfFile(TrfFile&& other) noexcept : file_(other.file_) {
        other.file_ = nullptr;
    }

    TrfFile& TrfFile::operator=(TrfFile&& other) noexcept {
        if (this != &other) {
            close();
            file_ = other.file_;
            other.file_ = nullptr;
        }
        return *this;
    }

    void TrfFile::close() noexcept {
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    std::expected<TrfFile, TrfError> TrfFile::open(const std::filesystem::path& path) {
        TrfFile file;
        file.file_ = open_handle(path, "r+b");
        if (file.file_ == nullptr) {
            const auto code = errno == ENOENT ? TrfErrorCode::NotFound : TrfErrorCode::Io;
            return std::unexpected(make_error(code, system_error_detail("cannot open region")));
        }
        return file;
    }

    std::expected<void, TrfError> TrfFile::create(const std::filesystem::path& path,
        std::uint64_t size) {

        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            return {};
        }

        std::FILE* handle = open_handle(path, "w+b");
        if (handle == nullptr) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("cannot create region")));
        }

#ifdef _WIN32
        const intptr_t raw = _get_osfhandle(_fileno(handle));
        if (raw != -1) {
            HANDLE device = reinterpret_cast<HANDLE>(raw);
            DWORD returned = 0;
            // Sparse before the extension; otherwise the file becomes dense.
            DeviceIoControl(device, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
        }
#endif

        if (auto sized = set_file_size(handle, size); !sized) {
            std::fclose(handle);
            std::filesystem::remove(path, ec);
            return sized;
        }

        std::fflush(handle);
        std::fclose(handle);
        return {};
    }

    std::expected<void, TrfError> TrfFile::replace(const std::filesystem::path& from,
        const std::filesystem::path& to) {

#ifdef _WIN32
        if (MoveFileExW(wide_path(from).c_str(), wide_path(to).c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return {};
        }
        return std::unexpected(make_error(TrfErrorCode::Io, "cannot replace region file"));
#else
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
        if (ec) {
            return std::unexpected(make_error(TrfErrorCode::Io, "cannot replace region file"));
        }
        return {};
#endif
    }

    std::expected<void, TrfError> TrfFile::remove(const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            return std::unexpected(make_error(TrfErrorCode::Io, "cannot remove file"));
        }
        return {};
    }

    std::expected<void, TrfError> TrfFile::sync_directory(const std::filesystem::path& directory) {
#ifdef _WIN32
        (void)directory;
        return {};
#else
        const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("cannot open directory")));
        }
        const bool ok = ::fsync(fd) == 0;
        ::close(fd);
        if (!ok) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("cannot sync directory")));
        }
        return {};
#endif
    }

    std::expected<void, TrfError> TrfFile::read_at(std::uint64_t offset,
        std::span<std::byte> out) const {

        if (file_ == nullptr) {
            return std::unexpected(make_error(TrfErrorCode::Io, "read from a closed region"));
        }
#ifdef _WIN32
        if (_fseeki64(file_, static_cast<__int64>(offset), SEEK_SET) != 0) {
#else
        if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) {
#endif
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("seek failed")));
        }
        if (out.empty()) {
            return {};
        }
        const std::size_t read = std::fread(out.data(), 1, out.size(), file_);
        if (read != out.size()) {
            return std::unexpected(make_error(TrfErrorCode::Io, "short read: expected " +
                std::to_string(out.size()) + " bytes, got " + std::to_string(read)));
        }
        return {};
    }

    std::expected<void, TrfError> TrfFile::write_at(std::uint64_t offset,
        std::span<const std::byte> data) {

        if (file_ == nullptr) {
            return std::unexpected(make_error(TrfErrorCode::Io, "write to a closed region"));
        }
#ifdef _WIN32
        if (_fseeki64(file_, static_cast<__int64>(offset), SEEK_SET) != 0) {
#else
        if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) {
#endif
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("seek failed")));
        }
        if (data.empty()) {
            return {};
        }
        const std::size_t written = std::fwrite(data.data(), 1, data.size(), file_);
        if (written != data.size()) {
            return std::unexpected(make_error(TrfErrorCode::Io, "short write: expected " +
                std::to_string(data.size()) + " bytes, got " + std::to_string(written)));
        }
        return {};
    }

    std::expected<void, TrfError> TrfFile::resize(std::uint64_t size) {
        if (file_ == nullptr) {
            return std::unexpected(make_error(TrfErrorCode::Io, "resize on a closed region"));
        }
        if (std::fflush(file_) != 0) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("flush failed")));
        }
        return set_file_size(file_, size);
    }

    std::expected<void, TrfError> TrfFile::lock_exclusive() {
        if (file_ == nullptr) {
            return std::unexpected(make_error(TrfErrorCode::Io, "lock on a closed region"));
        }
#ifdef _WIN32
        const intptr_t raw = _get_osfhandle(_fileno(file_));
        if (raw == -1) {
            return std::unexpected(make_error(TrfErrorCode::Io, "lock: no file handle"));
        }
        OVERLAPPED overlapped{};
        if (!LockFileEx(reinterpret_cast<HANDLE>(raw),
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)) {
            const DWORD error = GetLastError();
            if (error == ERROR_LOCK_VIOLATION || error == ERROR_IO_PENDING) {
                return std::unexpected(make_error(TrfErrorCode::Locked,
                    "another process holds the region lock"));
            }
            return std::unexpected(make_error(TrfErrorCode::Io,
                "cannot lock region (windows error " + std::to_string(error) + ")"));
        }
        return {};
#else
        if (flock(fileno(file_), LOCK_EX | LOCK_NB) != 0) {
            return std::unexpected(make_error(
                errno == EWOULDBLOCK ? TrfErrorCode::Locked : TrfErrorCode::Io,
                system_error_detail("cannot lock region")));
        }
        return {};
#endif
    }

    std::expected<void, TrfError> TrfFile::flush_stream() {
        if (file_ == nullptr) {
            return std::unexpected(make_error(TrfErrorCode::Io, "flush on a closed region"));
        }
        if (std::fflush(file_) != 0) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("flush failed")));
        }
        return {};
    }

    std::expected<void, TrfError> TrfFile::barrier() {
        if (file_ == nullptr) {
            return std::unexpected(make_error(TrfErrorCode::Io, "barrier on a closed region"));
        }
        if (std::fflush(file_) != 0) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("flush failed")));
        }
#ifdef _WIN32
        if (_commit(_fileno(file_)) != 0) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("barrier failed")));
        }
#else
        if (fdatasync(fileno(file_)) != 0) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("barrier failed")));
        }
#endif
        return {};
    }

    std::expected<std::uint64_t, TrfError> TrfFile::size() const {
        if (file_ == nullptr) {
            return std::unexpected(make_error(TrfErrorCode::Io, "size of a closed region"));
        }
#ifdef _WIN32
        const __int64 current = _ftelli64(file_);
        if (current < 0) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("tell failed")));
        }
        if (_fseeki64(file_, 0, SEEK_END) != 0) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("seek failed")));
        }
        const __int64 end = _ftelli64(file_);
        _fseeki64(file_, current, SEEK_SET);
        if (end < 0) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("tell failed")));
        }
        return static_cast<std::uint64_t>(end);
#else
        const off_t current = ftello(file_);
        if (current < 0) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("tell failed")));
        }
        if (fseeko(file_, 0, SEEK_END) != 0) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("seek failed")));
        }
        const off_t end = ftello(file_);
        fseeko(file_, current, SEEK_SET);
        if (end < 0) {
            return std::unexpected(make_error(TrfErrorCode::Io, system_error_detail("tell failed")));
        }
        return static_cast<std::uint64_t>(end);
#endif
    }

}
