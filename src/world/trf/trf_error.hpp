#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace terila::world::trf {

    // §20: every fallible entry point reports through this value type, so TRF can be
    // called from noexcept code and no exception ever crosses the API boundary.
    enum class TrfErrorCode : std::uint8_t {
        None,
        Io,
        NotFound,
        AccessDenied,
        Locked,
        ReadOnly,
        UnsupportedFormat,
        UnsupportedFilesystem,
        CorruptHeader,
        CorruptFooter,
        CrcMismatch,
        ShaMismatch,
        TooLarge,
        CoordinateMismatch,
        WorldMismatch,
        GeneratorMismatch,
        CompressionFailed,
        OutOfMemory,
        Busy,
        Cancelled,
    };

    struct TrfError {
        TrfErrorCode code = TrfErrorCode::None;
        int sys_errno = 0;
        std::uint32_t chunk_id = 0xFFFFFFFFu;
        std::int32_t cx = 0;
        std::int32_t cz = 0;
        std::string detail;
    };

    inline TrfError make_error(TrfErrorCode code, std::string detail) {
        TrfError error;
        error.code = code;
        error.detail = std::move(detail);
        return error;
    }

    inline TrfError make_chunk_error(TrfErrorCode code, std::uint32_t chunk_id, std::string detail) {
        TrfError error = make_error(code, std::move(detail));
        error.chunk_id = chunk_id;
        return error;
    }

    inline TrfError make_coordinate_error(TrfErrorCode code, std::int32_t cx, std::int32_t cz,
        std::string detail) {
        TrfError error = make_error(code, std::move(detail));
        error.cx = cx;
        error.cz = cz;
        return error;
    }

    inline const char* to_string(TrfErrorCode code) noexcept {
        switch (code) {
        case TrfErrorCode::None:                  return "None";
        case TrfErrorCode::Io:                    return "Io";
        case TrfErrorCode::NotFound:              return "NotFound";
        case TrfErrorCode::AccessDenied:          return "AccessDenied";
        case TrfErrorCode::Locked:                return "Locked";
        case TrfErrorCode::ReadOnly:              return "ReadOnly";
        case TrfErrorCode::UnsupportedFormat:     return "UnsupportedFormat";
        case TrfErrorCode::UnsupportedFilesystem: return "UnsupportedFilesystem";
        case TrfErrorCode::CorruptHeader:         return "CorruptHeader";
        case TrfErrorCode::CorruptFooter:         return "CorruptFooter";
        case TrfErrorCode::CrcMismatch:           return "CrcMismatch";
        case TrfErrorCode::ShaMismatch:           return "ShaMismatch";
        case TrfErrorCode::TooLarge:              return "TooLarge";
        case TrfErrorCode::CoordinateMismatch:    return "CoordinateMismatch";
        case TrfErrorCode::WorldMismatch:         return "WorldMismatch";
        case TrfErrorCode::GeneratorMismatch:     return "GeneratorMismatch";
        case TrfErrorCode::CompressionFailed:     return "CompressionFailed";
        case TrfErrorCode::OutOfMemory:           return "OutOfMemory";
        case TrfErrorCode::Busy:                  return "Busy";
        case TrfErrorCode::Cancelled:             return "Cancelled";
        }
        return "Unknown";
    }

}
