// The plugin gate is what decides whether unknown code runs, so it is tested without a
// WebAssembly runtime: every case below is about documents and directories only.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "plugin/capability.hpp"
#include "plugin/manifest.hpp"
#include "plugin/plugin_host.hpp"

namespace fs = std::filesystem;
using terila::plugin::Plugin;
using terila::plugin::PluginHost;

namespace {

    class TempTree {
    public:
        TempTree() {
            const auto unique = fs::temp_directory_path() /
                ("terila-plugin-test-" + std::to_string(::testing::UnitTest::GetInstance()
                    ->current_test_info()->line()));
            std::error_code ec;
            fs::remove_all(unique, ec);
            fs::create_directories(unique, ec);
            root_ = unique;
        }

        ~TempTree() {
            std::error_code ec;
            fs::remove_all(root_, ec);
        }

        const fs::path& root() const { return root_; }

        void write(const fs::path& relative, const nlohmann::json& document) {
            const fs::path path = root_ / relative;
            fs::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << document.dump(2);
        }

        void write_text(const fs::path& relative, const std::string& text) {
            const fs::path path = root_ / relative;
            fs::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << text;
        }

    private:
        fs::path root_;
    };

    nlohmann::json manifest_of(const std::string& id) {
        return nlohmann::json{
            { "schema", 1 },
            { "api_version", 1 },
            { "id", id },
            { "name", "Demo" },
            { "version", "0.1.0" },
            { "module", "plugin.wasm" },
        };
    }

    nlohmann::json request_of(const std::string& id, std::vector<std::string> requested) {
        return nlohmann::json{
            { "schema", 1 },
            { "api_version", 1 },
            { "id", id },
            { "requested", std::move(requested) },
        };
    }

    nlohmann::json grant_of(const std::string& id, std::vector<std::string> granted) {
        return nlohmann::json{ { "id", id }, { "granted", std::move(granted) } };
    }

    nlohmann::json grant_with_limits(const std::string& id, std::vector<std::string> granted,
        int fuel, int events) {
        nlohmann::json document = grant_of(id, std::move(granted));
        document["limits"] = { { "fuel_per_call", fuel }, { "events_per_second", events } };
        return document;
    }

    const Plugin* find_plugin(const std::string& id) {
        for (const Plugin& plugin : PluginHost::instance().plugins()) {
            if (plugin.manifest.id == id) return &plugin;
        }
        return nullptr;
    }

} // namespace

TEST(PluginCapability, RejectsNamesTheRegistryDoesNotHave) {
    EXPECT_TRUE(terila::plugin::is_known_capability("world.read"));
    EXPECT_FALSE(terila::plugin::is_known_capability("world.write.everything"));
    EXPECT_FALSE(terila::plugin::is_known_capability("*"));
    EXPECT_EQ(terila::plugin::first_unknown_capability({ "log.write", "nope" }), "nope");
}

TEST(PluginCapability, EffectiveIsTheIntersection) {
    const std::vector<std::string> requested{ "log.write", "chat.send", "world.write" };
    const std::vector<std::string> granted{ "chat.send", "world.write", "player.kick" };
    const auto effective = terila::plugin::effective_capabilities(requested, granted);
    ASSERT_EQ(effective.size(), 2u);
    EXPECT_EQ(effective[0], "chat.send");
    EXPECT_EQ(effective[1], "world.write");
}

TEST(PluginCapability, IdsThatWouldEscapeThePackAreRejected) {
    EXPECT_TRUE(terila::plugin::is_valid_plugin_id("demo.plugin-1"));
    EXPECT_FALSE(terila::plugin::is_valid_plugin_id("Demo"));
    EXPECT_FALSE(terila::plugin::is_valid_plugin_id("../demo"));
    EXPECT_FALSE(terila::plugin::is_valid_plugin_id("demo..plugin"));
    EXPECT_FALSE(terila::plugin::is_valid_plugin_id("ab"));
}

TEST(PluginGrant, LimitsSetTheBudgetsAndAreChecked) {
    nlohmann::json document = grant_with_limits("demo", { "log.write" }, 5000, 7);
    const auto result = terila::plugin::parse_grant(document);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.limits.fuel_per_call, 5000);
    EXPECT_EQ(result.limits.events_per_second, 7);

    document["limits"]["fuel_per_call"] = 0;
    const auto refused_fuel = terila::plugin::parse_grant(document);
    ASSERT_FALSE(refused_fuel);
    EXPECT_EQ(refused_fuel.problem->field, "limits.fuel_per_call");

    document = grant_with_limits("demo", { "log.write" }, 5000, 0);
    const auto refused_events = terila::plugin::parse_grant(document);
    ASSERT_FALSE(refused_events);
    EXPECT_EQ(refused_events.problem->field, "limits.events_per_second");
}

TEST(PluginGrant, WithoutLimitsTheDefaultsApply) {
    const auto result = terila::plugin::parse_grant(grant_of("demo", { "log.write" }));
    ASSERT_TRUE(result);
    EXPECT_EQ(result.limits.fuel_per_call, terila::plugin::kDefaultFuelPerCall);
    EXPECT_EQ(result.limits.events_per_second, terila::plugin::kDefaultEventsPerSecond);
}

TEST(PluginManifest, UnknownCapabilityIsRefused) {
    const auto result = terila::plugin::parse_request(request_of("demo", { "world.nuke" }));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.problem->field, "requested");
    EXPECT_NE(result.problem->text().find("world.nuke"), std::string::npos);
}

TEST(PluginManifest, DuplicatedCapabilityIsRefused) {
    const auto result = terila::plugin::parse_request(
        request_of("demo", { "log.write", "log.write" }));
    ASSERT_FALSE(result);
}

TEST(PluginManifest, ModuleCannotLeaveThePackage) {
    nlohmann::json document = manifest_of("demo");
    document["module"] = "../../etc/passwd";
    EXPECT_FALSE(terila::plugin::parse_manifest(document));
    document["module"] = "C:/windows/system32/x.wasm";
    EXPECT_FALSE(terila::plugin::parse_manifest(document));
    document["module"] = "nested/plugin.wasm";
    EXPECT_TRUE(terila::plugin::parse_manifest(document));
}

TEST(PluginManifest, MemoryCeilingIsEnforced) {
    nlohmann::json document = manifest_of("demo");
    document["memory_pages"] = 4096;
    EXPECT_FALSE(terila::plugin::parse_manifest(document));
    document["memory_pages"] = 128;
    const auto result = terila::plugin::parse_manifest(document);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.manifest.memory_pages, 128u);
}

TEST(PluginHost, FolderWithoutPermissionRequestIsNotAPlugin) {
    TempTree tree;
    tree.write("packs/notaplugin/plugin.json", manifest_of("notaplugin"));

    PluginHost::instance().load_all(tree.root() / "packs", tree.root() / "grants");
    EXPECT_EQ(find_plugin("notaplugin"), nullptr);
}

TEST(PluginHost, MissingGrantDisablesAnOtherwiseValidPackage) {
    TempTree tree;
    tree.write("packs/demo/plugin.json", manifest_of("demo"));
    tree.write("packs/demo/permissions.json", request_of("demo", { "log.write" }));

    PluginHost::instance().load_all(tree.root() / "packs", tree.root() / "grants");
    const Plugin* plugin = find_plugin("demo");
    ASSERT_NE(plugin, nullptr);
    EXPECT_TRUE(plugin->accepted);
    EXPECT_FALSE(plugin->enabled);
    EXPECT_NE(plugin->status.find("no grant file"), std::string::npos);
}

TEST(PluginHost, APluginCannotGrantItselfAnything) {
    TempTree tree;
    tree.write("packs/demo/plugin.json", manifest_of("demo"));
    tree.write("packs/demo/permissions.json", request_of("demo", { "world.write" }));

    PluginHost::instance().load_all(tree.root() / "packs", tree.root() / "grants");
    const Plugin* plugin = find_plugin("demo");
    ASSERT_NE(plugin, nullptr);
    EXPECT_FALSE(plugin->enabled);
    EXPECT_TRUE(plugin->effective.empty());
}

TEST(PluginHost, GrantNarrowsTheRequest) {
    TempTree tree;
    tree.write("packs/demo/plugin.json", manifest_of("demo"));
    tree.write("packs/demo/permissions.json",
        request_of("demo", { "log.write", "chat.send" }));
    tree.write("grants/demo.json", grant_of("demo", { "log.write", "player.kick" }));

    PluginHost::instance().load_all(tree.root() / "packs", tree.root() / "grants");
    const Plugin* plugin = find_plugin("demo");
    ASSERT_NE(plugin, nullptr);
    ASSERT_EQ(plugin->effective.size(), 1u);
    EXPECT_EQ(plugin->effective[0], "log.write");
}

TEST(PluginHost, FolderMustMatchTheId) {
    TempTree tree;
    tree.write("packs/other/plugin.json", manifest_of("demo"));
    tree.write("packs/other/permissions.json", request_of("demo", { "log.write" }));

    PluginHost::instance().load_all(tree.root() / "packs", tree.root() / "grants");
    const Plugin* plugin = find_plugin("demo");
    ASSERT_NE(plugin, nullptr);
    EXPECT_FALSE(plugin->accepted);
    EXPECT_NE(plugin->status.find("folder name"), std::string::npos);
}

TEST(PluginHost, RequestMustNameTheManifestId) {
    TempTree tree;
    tree.write("packs/demo/plugin.json", manifest_of("demo"));
    tree.write("packs/demo/permissions.json", request_of("other", { "log.write" }));

    PluginHost::instance().load_all(tree.root() / "packs", tree.root() / "grants");
    const Plugin* plugin = find_plugin("demo");
    ASSERT_NE(plugin, nullptr);
    EXPECT_FALSE(plugin->accepted);
}

TEST(PluginHost, MissingPacksDirectoryIsNotAnError) {
    TempTree tree;
    PluginHost::instance().load_all(tree.root() / "packs", tree.root() / "grants");
    EXPECT_TRUE(PluginHost::instance().plugins().empty());
}

#if defined(TERILA_HAVE_WASMTIME)

namespace {

    void section(std::string& out, unsigned char id, const std::string& payload) {
        out.push_back(static_cast<char>(id));
        out.push_back(static_cast<char>(payload.size()));
        out += payload;
    }

    // The smallest module that exercises the lifecycle hook: one (i32, i32) -> i32 function
    // exported as on_load, the shape the host calls it with. `traps` makes it execute
    // `unreachable`, which is how a plugin refuses to start; otherwise it returns 0. Hand
    // encoded so the test needs no toolchain.
    std::string module_with_on_load(bool traps) {
        std::string bytes("\0asm\1\0\0\0", 8);
        section(bytes, 1, std::string("\1\x60\2\x7f\x7f\1\x7f", 7));  // (i32, i32) -> i32
        section(bytes, 3, std::string("\1\0", 2));                // one function, type 0
        section(bytes, 7, std::string("\1\7on_load\0\0", 11));    // export "on_load"
        if (traps) {
            section(bytes, 10, std::string("\1\3\0\0\xb", 5));    // unreachable
        } else {
            section(bytes, 10, std::string("\1\4\0\x41\0\xb", 6)); // i32.const 0
        }
        return bytes;
    }

    // A module that uses the ABI: it imports terila.log_write, keeps one page of memory with a
    // line of text at offset 16, and returns the status of the last call from on_load. Two
    // calls in one hook are how the event budget is observed within a single call.
    std::string module_that_logs(int calls = 1) {
        std::string bytes("\0asm\1\0\0\0", 8);
        section(bytes, 1, std::string("\x01\x60\x02\x7f\x7f\x01\x7f", 7));  // (i32,i32)->i32
        section(bytes, 2, std::string("\x01\x06terila\x09log_write\x00\x00", 20));
        section(bytes, 3, std::string("\x01\x00", 2));                       // one function
        section(bytes, 5, std::string("\x01\x00\x01", 3));                   // one memory
        section(bytes, 7, std::string("\x02\x06memory\x02\x00\x07on_load\x00\x01", 20));

        std::string body("\x00", 1);  // no locals
        for (int i = 0; i < calls; ++i) {
            if (i > 0) body.push_back('\x1a');  // drop the status of the earlier call
            body += std::string("\x41\x10\x41\x11\x10\x00", 6);  // log_write(16, 16)
        }
        body.push_back('\x0b');
        std::string code("\x01", 1);
        code.push_back(static_cast<char>(body.size()));
        code += body;
        section(bytes, 10, code);

        std::string data("\x01\x00\x41\x10\x0b\x10", 6);
        data += "hello from wasm\n";
        section(bytes, 11, data);
        return bytes;
    }

    // A module with one hook that accepts and one function that always traps, so the trap
    // policy can be driven without a real event.
    std::string module_with_a_trap() {
        std::string bytes("\0asm\1\0\0\0", 8);
        section(bytes, 1, std::string("\x01\x60\x02\x7f\x7f\x01\x7f", 7));  // (i32,i32)->i32
        section(bytes, 3, std::string("\x02\x00\x00", 3));  // on_load and boom, type 0
        section(bytes, 5, std::string("\x01\x00\x01", 3));  // one memory
        // The name is concatenated because 'b' would continue a \x escape.
        section(bytes, 7, std::string("\x03\x06memory\x02\x00\x07on_load\x00\x00", 20) +
                std::string("\x04" "boom\x00\x01", 7));
        section(bytes, 10,
            std::string("\x02\x04\x00\x41\x00\x0b\x03\x00\x00\x0b", 10));
        return bytes;
    }

} // namespace

// A plugin that keeps trapping is released: the call is dropped, the count is kept, and once
// it reaches kMaxTraps the instance is gone instead of failing on every event.
TEST(PluginHostRuntime, RepeatedTrapsDisableThePlugin) {
    TempTree tree;
    tree.write("packs/boomy/plugin.json", manifest_of("boomy"));
    tree.write("packs/boomy/permissions.json", request_of("boomy", { "log.write" }));
    tree.write("grants/boomy.json", grant_of("boomy", { "log.write" }));
    tree.write_text("packs/boomy/plugin.wasm", module_with_a_trap());

    PluginHost& host = PluginHost::instance();
    host.load_all(tree.root() / "packs", tree.root() / "grants");
    if (!host.has_runtime()) {
        GTEST_SKIP() << "no WebAssembly runtime was found; set TERILA_WASM_RUNTIME";
    }

    const Plugin* plugin = find_plugin("boomy");
    ASSERT_NE(plugin, nullptr);
    ASSERT_TRUE(plugin->enabled) << plugin->status;
    EXPECT_EQ(host.live_stores(), 1u);

    std::int32_t result = 0;
    std::string error;
    for (std::int32_t i = 0; i < terila::plugin::kMaxTraps; ++i) {
        ASSERT_FALSE(host.call("boomy", "boom", 1, 0, result, error));
        ASSERT_EQ(error, "trapped");
    }

    EXPECT_EQ(host.live_stores(), 0u);
    const Plugin* disabled = find_plugin("boomy");
    ASSERT_NE(disabled, nullptr);
    EXPECT_FALSE(disabled->enabled);
    EXPECT_EQ(disabled->status, "disabled: it trapped " +
        std::to_string(terila::plugin::kMaxTraps) + " times");

    EXPECT_FALSE(host.call("boomy", "boom", 1, 0, result, error));
    EXPECT_EQ(error, "not loaded");
}

// The import layer is the security boundary: a granted function is linked in and the call
// crosses memory correctly, so on_load returns the host's own status.
TEST(PluginHostRuntime, LogWriteImportIsLinkedAndCallable) {
    TempTree tree;
    tree.write("packs/logging/plugin.json", manifest_of("logging"));
    tree.write("packs/logging/permissions.json", request_of("logging", { "log.write" }));
    tree.write("grants/logging.json", grant_of("logging", { "log.write" }));
    tree.write_text("packs/logging/plugin.wasm", module_that_logs());

    PluginHost& host = PluginHost::instance();
    host.load_all(tree.root() / "packs", tree.root() / "grants");
    if (!host.has_runtime()) {
        GTEST_SKIP() << "no WebAssembly runtime was found; set TERILA_WASM_RUNTIME";
    }

    const Plugin* plugin = find_plugin("logging");
    ASSERT_NE(plugin, nullptr);
    ASSERT_TRUE(plugin->enabled) << plugin->status;
    EXPECT_EQ(host.live_stores(), 1u);

    std::int32_t result = -1;
    std::string error;
    EXPECT_TRUE(host.call("logging", "on_load", 1, 0, result, error)) << error;
    EXPECT_EQ(result, 0);
}

// A capability the plugin does not hold is not linked, so the module cannot start at all and
// the refusal names the import.
TEST(PluginHostRuntime, AnImportThatWasNotGrantedIsRefusedBeforeItRuns) {
    TempTree tree;
    tree.write("packs/asking/plugin.json", manifest_of("asking"));
    tree.write("packs/asking/permissions.json", request_of("asking", { "chat.send" }));
    tree.write("grants/asking.json", grant_of("asking", { "chat.send" }));
    tree.write_text("packs/asking/plugin.wasm", module_that_logs());

    PluginHost& host = PluginHost::instance();
    host.load_all(tree.root() / "packs", tree.root() / "grants");
    if (!host.has_runtime()) {
        GTEST_SKIP() << "no WebAssembly runtime was found; set TERILA_WASM_RUNTIME";
    }

    const Plugin* plugin = find_plugin("asking");
    ASSERT_NE(plugin, nullptr);
    EXPECT_FALSE(plugin->enabled);
    EXPECT_NE(plugin->status.find("log_write"), std::string::npos) << plugin->status;
    EXPECT_EQ(host.live_stores(), 0u);
}

// Fuel is the grant's number, not a constant in the host: one unit is not enough to run
// on_load, and a hook that overruns is refused like any other trap.
TEST(PluginHostRuntime, FuelPerCallStopsARunawayHook) {
    TempTree tree;
    tree.write("packs/thrifty/plugin.json", manifest_of("thrifty"));
    tree.write("packs/thrifty/permissions.json", request_of("thrifty", { "log.write" }));
    tree.write("grants/thrifty.json", grant_with_limits("thrifty", { "log.write" }, 1, 100));
    tree.write_text("packs/thrifty/plugin.wasm", module_that_logs());

    PluginHost& host = PluginHost::instance();
    host.load_all(tree.root() / "packs", tree.root() / "grants");
    if (!host.has_runtime()) {
        GTEST_SKIP() << "no WebAssembly runtime was found; set TERILA_WASM_RUNTIME";
    }

    const Plugin* plugin = find_plugin("thrifty");
    ASSERT_NE(plugin, nullptr);
    EXPECT_EQ(plugin->fuel_per_call, 1);
    EXPECT_FALSE(plugin->enabled);
    EXPECT_EQ(plugin->status, "refused: on_load trapped");
    EXPECT_EQ(host.live_stores(), 0u);
}

// The event budget is the grant's too: the second line inside one hook is dropped with the
// documented -2 instead of being written.
TEST(PluginHostRuntime, LogBudgetDropsTheExtraLine) {
    TempTree tree;
    tree.write("packs/chatty/plugin.json", manifest_of("chatty"));
    tree.write("packs/chatty/permissions.json", request_of("chatty", { "log.write" }));
    tree.write("grants/chatty.json", grant_with_limits("chatty", { "log.write" },
        terila::plugin::kDefaultFuelPerCall, 1));
    tree.write_text("packs/chatty/plugin.wasm", module_that_logs(2));

    PluginHost& host = PluginHost::instance();
    host.load_all(tree.root() / "packs", tree.root() / "grants");
    if (!host.has_runtime()) {
        GTEST_SKIP() << "no WebAssembly runtime was found; set TERILA_WASM_RUNTIME";
    }

    const Plugin* plugin = find_plugin("chatty");
    ASSERT_NE(plugin, nullptr);
    ASSERT_TRUE(plugin->enabled) << plugin->status;
    EXPECT_EQ(plugin->events_per_second, 1);

    std::int32_t result = 0;
    std::string error;
    EXPECT_TRUE(host.call("chatty", "on_load", 1, 0, result, error)) << error;
    EXPECT_EQ(result, -2);
}

// The leak this guards against: an instance registered before on_load ran stayed in the host
// after the hook was refused, so it kept its store and was still reachable through call().
TEST(PluginHostRuntime, RefusedLifecycleHookLeavesNoStoreOrInstance) {
    TempTree tree;
    tree.write("packs/refusing/plugin.json", manifest_of("refusing"));
    tree.write("packs/refusing/permissions.json", request_of("refusing", { "log.write" }));
    tree.write("grants/refusing.json", grant_of("refusing", { "log.write" }));
    tree.write_text("packs/refusing/plugin.wasm", module_with_on_load(true));

    PluginHost& host = PluginHost::instance();
    host.load_all(tree.root() / "packs", tree.root() / "grants");
    if (!host.has_runtime()) {
        GTEST_SKIP() << "no WebAssembly runtime was found; set TERILA_WASM_RUNTIME";
    }

    const Plugin* plugin = find_plugin("refusing");
    ASSERT_NE(plugin, nullptr);
    EXPECT_FALSE(plugin->enabled);
    EXPECT_EQ(plugin->status, "refused: on_load trapped");

    // Reachable would mean the refused instance is still alive in the host.
    std::int32_t result = 0;
    std::string error;
    EXPECT_FALSE(host.call("refusing", "on_load", 1, 0, result, error));
    EXPECT_EQ(error, "not loaded");
    EXPECT_EQ(host.live_stores(), 0u);
}

// The other half of the proof: an accepted plugin really does hold one store, so a count of
// zero above is not the counter being blind.
TEST(PluginHostRuntime, EnabledPluginHoldsOneStoreUntilUnload) {
    TempTree tree;
    tree.write("packs/working/plugin.json", manifest_of("working"));
    tree.write("packs/working/permissions.json", request_of("working", { "log.write" }));
    tree.write("grants/working.json", grant_of("working", { "log.write" }));
    tree.write_text("packs/working/plugin.wasm", module_with_on_load(false));

    PluginHost& host = PluginHost::instance();
    host.load_all(tree.root() / "packs", tree.root() / "grants");
    if (!host.has_runtime()) {
        GTEST_SKIP() << "no WebAssembly runtime was found; set TERILA_WASM_RUNTIME";
    }

    const Plugin* plugin = find_plugin("working");
    ASSERT_NE(plugin, nullptr);
    ASSERT_TRUE(plugin->enabled) << plugin->status;
    EXPECT_EQ(host.live_stores(), 1u);

    std::int32_t result = 1;
    std::string error;
    EXPECT_TRUE(host.call("working", "on_load", 1, 0, result, error));
    EXPECT_EQ(result, 0);

    host.unload_all();
    EXPECT_EQ(host.live_stores(), 0u);
    EXPECT_TRUE(host.plugins().empty());
}

#endif // TERILA_HAVE_WASMTIME
