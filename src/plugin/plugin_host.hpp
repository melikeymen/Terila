#pragma once

// The plugin host: it decides what is a plugin and what each one may do, and (when the build
// has a WebAssembly runtime) it turns the surviving packages into callable instances.
//
// The gate is the important half and it never depends on the runtime:
//
//   1. a pack is a plugin only when it has BOTH plugin.json and permissions.json
//   2. plugin.json must be schema 1, api_version 1, a valid id, and the folder must be
//      named after that id
//   3. permissions.json must name the same id and only capabilities this build has
//   4. the grant lives in the host's own directory; without one the plugin is disabled
//   5. what the plugin holds is requested INTERSECT granted, never the request
//
// A refusal is reported, never silently dropped, so an operator can see why a package did
// not start.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/manifest.hpp"

namespace terila::plugin {

    struct Plugin {
        std::filesystem::path directory;
        Manifest manifest;
        std::vector<std::string> requested;
        std::vector<std::string> granted;
        std::vector<std::string> effective;

        // The package passed every gate. A plugin can be accepted and still not enabled,
        // which is what a missing grant or a build without a runtime looks like.
        bool accepted = false;
        bool enabled = false;
        std::string status;  // "enabled" or one printable line saying why it is not
    };

    class PluginHost {
    public:
        static PluginHost& instance();

        // Scans `packs` for packages, reads `grants` (one file per plugin id) and loads what
        // survived. A missing directory is not an error: plugins are optional.
        void load_all(const std::filesystem::path& packs, const std::filesystem::path& grants);
        void unload_all();

        const std::vector<Plugin>& plugins() const noexcept { return plugins_; }
        bool has_runtime() const noexcept { return library_ != nullptr; }
        const std::string& runtime_name() const noexcept { return runtime_; }

        // Calls an exported i32(i32, i32) function on an enabled plugin. Lifecycle hooks and
        // future host calls share this one path.
        bool call(std::string_view id, std::string_view function, std::int32_t first,
            std::int32_t second, std::int32_t& result, std::string& error);

    private:
        PluginHost() = default;
        PluginHost(const PluginHost&) = delete;
        PluginHost& operator=(const PluginHost&) = delete;

        void load_one(const std::filesystem::path& directory,
            const std::filesystem::path& grants);

        std::vector<Plugin> plugins_;
        void* library_ = nullptr;
        void* engine_ = nullptr;
        std::string runtime_;
    };

} // namespace terila::plugin
