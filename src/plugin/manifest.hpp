#pragma once

// Parsing and validation of the three JSON documents the plugin system reads:
//
//   <pack>/plugin.json              what the package is (its identity and its module)
//   <pack>/permissions.json         what it asks for; without this file it is not a plugin
//   config/plugin-grants/<id>.json  what the operator allows, owned by the host
//
// Everything here is pure: a document in, a struct and at most one problem out. No file is
// touched and nothing is loaded, which is what makes the gate testable without a runtime.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "plugin/capability.hpp"

namespace terila::plugin {

    inline constexpr int kSchema = 1;
    inline constexpr int kApiVersion = 1;
    inline constexpr std::uint32_t kDefaultMemoryPages = 64;

    // One refusal, carrying the field it is about so an operator can fix the file without
    // guessing.
    struct Problem {
        std::string field;
        std::string reason;

        std::string text() const {
            if (field.empty()) return reason;
            return field + ": " + reason;
        }
    };

    struct Manifest {
        std::string id;
        std::string name;
        std::string version;
        std::string module = "plugin.wasm";
        std::string description;
        int schema = kSchema;
        int api_version = kApiVersion;
        std::uint32_t memory_pages = kDefaultMemoryPages;
    };

    struct ManifestResult {
        Manifest manifest;
        std::optional<Problem> problem;

        explicit operator bool() const noexcept { return !problem.has_value(); }
    };

    struct RequestResult {
        std::string id;
        std::vector<std::string> requested;
        std::optional<Problem> problem;

        explicit operator bool() const noexcept { return !problem.has_value(); }
    };

    struct GrantResult {
        std::string id;
        std::vector<std::string> granted;
        std::optional<Problem> problem;

        explicit operator bool() const noexcept { return !problem.has_value(); }
    };

    namespace detail {

        inline bool read_string(const nlohmann::json& json, const char* key, std::string& out,
            std::optional<Problem>& problem, bool required) {
            const auto it = json.find(key);
            if (it == json.end() || it->is_null()) {
                if (required) {
                    problem = Problem{ key, "is required" };
                    return false;
                }
                return true;
            }
            if (!it->is_string()) {
                problem = Problem{ key, "must be a string" };
                return false;
            }
            out = it->get<std::string>();
            return true;
        }

        inline bool read_integer(const nlohmann::json& json, const char* key, int& out,
            std::optional<Problem>& problem, bool required) {
            const auto it = json.find(key);
            if (it == json.end() || it->is_null()) {
                if (required) {
                    problem = Problem{ key, "is required" };
                    return false;
                }
                return true;
            }
            if (!it->is_number_integer()) {
                problem = Problem{ key, "must be an integer" };
                return false;
            }
            out = it->get<int>();
            return true;
        }

        // A capability list is an array of strings. Unknown names are reported by the caller,
        // because a request may only use known names while a grant may be stale.
        inline bool read_capabilities(const nlohmann::json& json, const char* key,
            std::vector<std::string>& out, std::optional<Problem>& problem) {
            const auto it = json.find(key);
            if (it == json.end()) return true;
            if (!it->is_array()) {
                problem = Problem{ key, "must be an array of capability names" };
                return false;
            }
            for (const auto& item : *it) {
                if (!item.is_string()) {
                    problem = Problem{ key, "must contain only strings" };
                    return false;
                }
                out.push_back(item.get<std::string>());
            }
            return true;
        }

        // A module path is resolved inside the pack, so a package cannot name a file outside
        // its own directory. Absolute paths, drive letters, '..' and backslashes are refused.
        inline bool module_path_is_contained(std::string_view module) {
            if (module.empty() || module.size() > 200) return false;
            if (module.front() == '/' || module.front() == '\\') return false;
            if (module.size() > 1 && module[1] == ':') return false;
            std::size_t start = 0;
            while (start <= module.size()) {
                const std::size_t slash = module.find_first_of("/\\", start);
                const std::string_view part = module.substr(start,
                    slash == std::string_view::npos ? std::string_view::npos : slash - start);
                if (part == "..") return false;
                if (slash == std::string_view::npos) break;
                start = slash + 1;
            }
            return true;
        }

    } // namespace detail

    inline ManifestResult parse_manifest(const nlohmann::json& json) {
        ManifestResult result;
        if (!json.is_object()) {
            result.problem = Problem{ "", "plugin.json must contain an object" };
            return result;
        }

        std::optional<Problem>& problem = result.problem;
        Manifest& out = result.manifest;

        if (!detail::read_integer(json, "schema", out.schema, problem, false)) return result;
        if (out.schema != kSchema) {
            problem = Problem{ "schema", "this build reads schema " + std::to_string(kSchema) };
            return result;
        }
        if (!detail::read_integer(json, "api_version", out.api_version, problem, false)) {
            return result;
        }
        if (out.api_version != kApiVersion) {
            problem = Problem{ "api_version",
                "this build serves api_version " + std::to_string(kApiVersion) };
            return result;
        }

        if (!detail::read_string(json, "id", out.id, problem, true)) return result;
        if (!is_valid_plugin_id(out.id)) {
            problem = Problem{ "id", "'" + out.id + "' is not a valid plugin id" };
            return result;
        }
        if (!detail::read_string(json, "name", out.name, problem, true)) return result;
        if (!detail::read_string(json, "version", out.version, problem, true)) return result;
        if (!detail::read_string(json, "description", out.description, problem, false)) {
            return result;
        }

        std::string module;
        if (!detail::read_string(json, "module", module, problem, false)) return result;
        if (!module.empty()) {
            if (!detail::module_path_is_contained(module)) {
                problem = Problem{ "module", "must name a file inside the package" };
                return result;
            }
            out.module = module;
        }

        const auto pages = json.find("memory_pages");
        if (pages != json.end() && !pages->is_null()) {
            if (!pages->is_number_unsigned() && !pages->is_number_integer()) {
                problem = Problem{ "memory_pages", "must be an integer" };
                return result;
            }
            const auto value = pages->get<std::int64_t>();
            // 1 page is 64 KiB; the ceiling is the same one the design doc states, which
            // keeps a single plugin from reserving the host's whole address space.
            if (value < 1 || value > 1024) {
                problem = Problem{ "memory_pages", "must be between 1 and 1024" };
                return result;
            }
            out.memory_pages = static_cast<std::uint32_t>(value);
        }

        return result;
    }

    inline RequestResult parse_request(const nlohmann::json& json) {
        RequestResult result;
        if (!json.is_object()) {
            result.problem = Problem{ "", "permissions.json must contain an object" };
            return result;
        }

        std::optional<Problem>& problem = result.problem;

        int schema = kSchema;
        int api_version = kApiVersion;
        if (!detail::read_integer(json, "schema", schema, problem, false)) return result;
        if (schema != kSchema) {
            problem = Problem{ "schema", "this build reads schema " + std::to_string(kSchema) };
            return result;
        }
        if (!detail::read_integer(json, "api_version", api_version, problem, false)) return result;
        if (api_version != kApiVersion) {
            problem = Problem{ "api_version",
                "this build serves api_version " + std::to_string(kApiVersion) };
            return result;
        }
        if (!detail::read_string(json, "id", result.id, problem, true)) return result;
        if (!detail::read_capabilities(json, "requested", result.requested, problem)) {
            return result;
        }

        const std::string_view unknown = first_unknown_capability(result.requested);
        if (!unknown.empty()) {
            problem = Problem{ "requested", "'" + std::string(unknown) +
                "' is not a capability this build has" };
            return result;
        }
        for (std::size_t i = 0; i < result.requested.size(); ++i) {
            for (std::size_t j = i + 1; j < result.requested.size(); ++j) {
                if (result.requested[i] == result.requested[j]) {
                    problem = Problem{ "requested",
                        "'" + result.requested[i] + "' is listed twice" };
                    return result;
                }
            }
        }
        return result;
    }

    // A grant is the host's document, so an unknown name here is stale, not a refusal: it is
    // dropped, and a grant that grants nothing simply disables the plugin.
    inline GrantResult parse_grant(const nlohmann::json& json) {
        GrantResult result;
        if (!json.is_object()) {
            result.problem = Problem{ "", "a grant must contain an object" };
            return result;
        }
        std::optional<Problem>& problem = result.problem;
        if (!detail::read_string(json, "id", result.id, problem, true)) return result;

        std::vector<std::string> listed;
        if (!detail::read_capabilities(json, "granted", listed, problem)) return result;
        for (const std::string& name : listed) {
            if (is_known_capability(name)) result.granted.push_back(name);
        }
        return result;
    }

} // namespace terila::plugin
