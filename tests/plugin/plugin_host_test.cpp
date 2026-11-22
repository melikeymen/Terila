// The plugin gate is what decides whether unknown code runs, so it is tested without a
// WebAssembly runtime: every case below is about documents and directories only.

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
