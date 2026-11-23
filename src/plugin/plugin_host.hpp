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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "plugin/manifest.hpp"

namespace terila::plugin {

    // A plugin that keeps trapping is a broken plugin: the event is dropped, and after this
    // many traps the instance is released and the run continues without it.
    inline constexpr std::int32_t kMaxTraps = 32;

    // One online player as player.read reports it. The record is a fixed size so the
    // guest can size its own buffer, little-endian throughout:
    //
    //   0  i32  entity id
    //   4  name, 32 bytes, NUL padded (a longer name is truncated to 31 bytes)
    //   36 i32  dimension
    //   40 f64  x, 48 f64 y, 56 f64 z
    inline constexpr std::int32_t kPlayerRecordSize = 64;
    inline constexpr std::int32_t kPlayerNameSize = 32;

    struct PlayerRow {
        std::int32_t entity = 0;
        std::string name;
        std::int32_t dimension = 0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    // The server's answer to "who is online". Both halves must agree on one order (the server
    // sorts by entity id), so an index that count returned stays valid for read.
    struct PlayerSource {
        std::function<std::size_t()> count;
        std::function<bool(std::size_t index, PlayerRow& row)> read;
    };

    struct Plugin {
        std::filesystem::path directory;
        Manifest manifest;
        std::vector<std::string> requested;
        std::vector<std::string> granted;
        std::vector<std::string> effective;

        // The budgets the grant set, or the defaults when it said nothing.
        std::int32_t fuel_per_call = kDefaultFuelPerCall;
        std::int32_t events_per_second = kDefaultEventsPerSecond;

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

        // The server's one way out to the players: target 0 is everyone, a positive id is one
        // player, and the sink answers false when no such player is online. An empty function
        // means nothing is listening, and chat.send answers "unavailable".
        void set_chat_sink(std::function<bool(std::int32_t, std::string_view)> sink);

        // The server's view of who is online, for player.read. A missing source makes
        // the two player functions answer "unavailable" instead of failing a call.
        void set_player_source(PlayerSource source);

        const std::vector<Plugin>& plugins() const noexcept { return plugins_; }
        // Instances the host currently holds: one store per enabled plugin, none for a plugin
        // that was refused or unloaded.
        std::size_t live_stores() const noexcept;
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

        // Releases a plugin's instance and records why, so a trap run cannot keep calling it.
        void disable(std::string_view id, std::int32_t traps);

        std::vector<Plugin> plugins_;
        void* library_ = nullptr;
        void* engine_ = nullptr;
        std::string runtime_;
    };

} // namespace terila::plugin
