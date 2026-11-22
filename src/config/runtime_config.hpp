#pragma once

// server.properties holds the Minecraft-facing settings; how much of the machine the
// process may use is a different question. That half lives in configs/terila.config, which
// is written once on the first start and read back afterwards: the chunk pool, the chunk
// caches and the working-set cap all take their numbers from it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

namespace terila::config {

    // The file written inside the configs directory.
    inline constexpr const char* kRuntimeConfigFile = "terila.config";

    // Where the terrain's density field is evaluated. `Auto` asks the device once at start
    // and keeps the CPU when no OpenCL device can run the kernel; `Gpu` does the same but
    // also warns, because the operator asked for the device by name.
    enum class WorldgenBackend {
        Cpu,
        Gpu,
        Auto,
    };

    inline constexpr const char* worldgen_backend_name(WorldgenBackend backend) noexcept {
        switch (backend) {
        case WorldgenBackend::Gpu: return "gpu";
        case WorldgenBackend::Auto: return "auto";
        default: return "cpu";
        }
    }

    // Unknown spellings answer Cpu, which is also the default: a typo must not silently
    // turn the device path on.
    inline WorldgenBackend parse_worldgen_backend(std::string_view text) noexcept {
        if (text == "gpu") return WorldgenBackend::Gpu;
        if (text == "auto") return WorldgenBackend::Auto;
        return WorldgenBackend::Cpu;
    }

    struct RuntimeConfig {
        // Ceiling for the memory the server keeps around, in MiB. 0 disables the ceiling,
        // which leaves the caches on their sizing default.
        std::size_t max_memory_mb = 2048;
        // Memory kept free for the rest of the machine, in MiB. The chunk caches leave
        // this much of the ceiling unused. 0 disables the reserve.
        std::size_t min_memory_mb = 256;

        // Chunk generation workers. The pool starts between the two: never fewer than the
        // minimum, never more than the maximum. The ceiling is generous because a join has
        // to generate the whole view distance at once.
        unsigned max_threads = 16;
        unsigned min_threads = 2;

        // Where the chunk density field is evaluated. Cpu keeps everything on the workers.
        WorldgenBackend worldgen_backend = WorldgenBackend::Cpu;

        // What the caches may actually spend: the ceiling minus the reserve.
        std::size_t usable_memory_mb() const noexcept;

        // The worker count the pool starts with. One core is left to the io thread, so the
        // pool derives from the machine instead of always taking the maximum.
        unsigned worker_threads() const noexcept;
    };

    // The text of a freshly written terila.config, with the given values in place.
    std::string default_runtime_config_text(const RuntimeConfig& config);

    // Creates `directory` when it is missing and writes terila.config inside it when that
    // file is missing. An existing config is never rewritten. `written` receives the path
    // that was created, or stays empty when nothing had to be written.
    bool ensure_runtime_config(const std::filesystem::path& directory,
        const RuntimeConfig& defaults, std::filesystem::path* written = nullptr);

    // Reads the file; a missing or unreadable file leaves `out` untouched, and values out
    // of range fall back to the defaults with a warning.
    bool load_runtime_config(RuntimeConfig& out, const std::filesystem::path& path);

    // Applies the ceiling to the process working set where the platform allows it. The
    // caches are already sized from it, so a refusal only costs the hard cap.
    void apply_memory_limits(const RuntimeConfig& config);

    // How many columns one dimension's cache may hold inside the memory budget. A cached
    // column costs its block array plus its encoded payload, so the fit is computed from
    // the section count instead of guessing a count per view distance.
    std::size_t chunk_cache_capacity(const RuntimeConfig& config, int sections);

    namespace detail {

        // Below this a ceiling is nonsense for a server that holds a world in memory.
        inline constexpr std::size_t kMinMemoryMb = 256;
        // 1 TiB: anything above this is a typo, not a limit.
        inline constexpr std::size_t kMaxMemoryMb = std::size_t{ 1 } << 20;
        inline constexpr unsigned kMinThreads = 1;
        inline constexpr unsigned kMaxThreads = 64;

        // The budget used when the ceiling is disabled: the same 2 GiB a first start writes.
        inline constexpr std::size_t kSizingDefaultMb = 2048;

        // The chunk cache gets a third of the usable budget; the generators, the save
        // queues and the connections share the rest.
        inline constexpr std::size_t kCacheShares = 3;

        // A column of block states plus its encoded payload, which is what a cache entry
        // actually holds. The payload is estimated at 96 KiB, the size of an overworld
        // column with its light.
        inline constexpr std::size_t kEncodedBytesEstimate = 96 * 1024;

        inline void sanitize(RuntimeConfig& config) {
            if (config.max_memory_mb != 0) {
                config.max_memory_mb = std::clamp(config.max_memory_mb, kMinMemoryMb, kMaxMemoryMb);
            }
            if (config.min_memory_mb >= config.max_memory_mb && config.max_memory_mb != 0) {
                config.min_memory_mb = config.max_memory_mb / 4;
            }
            if (config.max_threads == 0) {
                config.max_threads = 1;
            }
            config.max_threads = std::clamp(config.max_threads, kMinThreads, kMaxThreads);
            config.min_threads = std::min(config.min_threads, config.max_threads);
        }

    } // namespace detail

    inline std::size_t RuntimeConfig::usable_memory_mb() const noexcept {
        const std::size_t ceiling =
            max_memory_mb == 0 ? detail::kSizingDefaultMb : max_memory_mb;
        return ceiling > min_memory_mb ? ceiling - min_memory_mb : ceiling / 2;
    }

    inline unsigned RuntimeConfig::worker_threads() const noexcept {
        const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
        const unsigned derived = hardware > 1 ? hardware - 1 : 1;
        const unsigned low = std::max(1u, std::min(min_threads, max_threads));
        const unsigned high = std::max(low, max_threads);
        return std::clamp(derived, low, high);
    }

    inline std::size_t chunk_cache_capacity(const RuntimeConfig& config, int sections) {
        const std::size_t per_dimension =
            std::max<std::size_t>(1, config.usable_memory_mb() / detail::kCacheShares);
        const std::size_t bytes_per_chunk =
            static_cast<std::size_t>(std::max(sections, 1)) * 4096 * sizeof(std::int32_t) +
            detail::kEncodedBytesEstimate;
        const std::size_t fit = (per_dimension * 1024 * 1024) / bytes_per_chunk;
        return std::clamp<std::size_t>(fit, 128, 8192);
    }

} // namespace terila::config
