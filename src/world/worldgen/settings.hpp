#pragma once

// The noise settings of a dimension: which noise fields the router names, where the sea
// level is, what the default blocks are and which material rule chain paints the surface.
// Everything the generator needs to describe the overworld lives here, read from the game's
// own JSON instead of being written into the code.

#include "data/input_store.hpp"
#include "world/worldgen/density/climate.hpp"
#include "world/worldgen/density/density.hpp"
#include "log/logger.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace terila::world::worldgen {

    class Settings {
    public:
        bool load(std::uint32_t seed, std::string_view preset = "minecraft:overworld",
            std::string_view biome_preset = "minecraft/overworld") {
            seed_ = seed;
            const nlohmann::json noise_settings = read_json("worldgen/noise_settings.json");
            if (noise_settings.is_null()) {
                return false;
            }

            const nlohmann::json* preset_data = find_preset(noise_settings, preset);
            if (preset_data == nullptr) {
                LOG_WARN(Chunk, "worldgen: preset '{}' is missing from noise_settings", preset);
                return false;
            }

            const nlohmann::json& data = *preset_data;
            if (data.contains("noise") && data["noise"].is_object()) {
                min_y_ = data["noise"].value("min_y", -64);
                height_ = data["noise"].value("height", 384);
            }
            sea_level_ = data.value("sea_level", 63);
            default_block_ = data.value("default_block", std::string("minecraft:stone"));
            default_fluid_ = data.value("default_fluid", std::string("minecraft:water"));
            material_rule_id_ = data.value("material_rule", std::string("minecraft:overworld"));

            const nlohmann::json functions = read_json("worldgen/density_functions.json");
            const nlohmann::json noises = read_json(
                "registries/dump/minecraft/worldgen/noise.json");
            if (functions.is_null() || noises.is_null()) {
                return false;
            }
            if (!density_.load(seed, functions, noises)) {
                LOG_WARN(Chunk, "worldgen: cannot load the density functions");
                return false;
            }

            // The End has no climate table: its biomes follow the island noise instead, so
            // an empty preset leaves the multi-noise source unused.
            if (!biome_preset.empty()) {
                const nlohmann::json parameters = read_json("worldgen/biome_parameters.json");
                if (parameters.is_null() || !climate_.load(parameters, biome_preset)) {
                    LOG_WARN(Chunk, "worldgen: cannot load the biome parameters of {}",
                        preset);
                    return false;
                }
                climate_ready_ = true;
            }

            material_rules_ = read_json("registries/dump/minecraft/worldgen/material_rule.json");
            material_conditions_ = read_json(
                "registries/dump/minecraft/worldgen/material_condition.json");
            if (material_rules_.is_null() || material_conditions_.is_null()) {
                return false;
            }

            if (data.contains("noise_router") && data["noise_router"].is_object()) {
                for (const auto& entry : data["noise_router"].items()) {
                    if (entry.value().is_string()) {
                        router_[entry.key()] = density_.find(entry.value().get<std::string>());
                    }
                    else if (!entry.value().is_null()) {
                        // Some dimensions spell a field out inline (the nether's temperature
                        // and vegetation are plain noise nodes) instead of naming it.
                        router_[entry.key()] = density_.compile(entry.value());
                    }
                }
            }

            final_density_ = root("final_density");
            if (!final_density_) {
                LOG_WARN(Chunk, "worldgen: the density router has no final_density");
                return false;
            }
            ready_ = true;
            return true;
        }

        bool ready() const noexcept { return ready_; }
        std::uint32_t seed() const noexcept { return seed_; }
        int min_y() const noexcept { return min_y_; }
        int height() const noexcept { return height_; }
        int world_top() const noexcept { return min_y_ + height_; }
        int sea_level() const noexcept { return sea_level_; }
        int section_count() const noexcept { return height_ / 16; }

        std::string_view default_block() const noexcept { return default_block_; }
        std::string_view default_fluid() const noexcept { return default_fluid_; }
        std::string_view material_rule_id() const noexcept { return material_rule_id_; }

        const DensityGraph& density() const noexcept { return density_; }
        const BiomeParameters& climate() const noexcept { return climate_; }
        bool has_climate() const noexcept { return climate_ready_; }
        const nlohmann::json& material_rules() const noexcept { return material_rules_; }
        const nlohmann::json& material_conditions() const noexcept {
            return material_conditions_;
        }

        DfPtr root(std::string_view name) const {
            const auto found = router_.find(std::string(name));
            return found == router_.end() ? nullptr : found->second;
        }

        DfPtr final_density() const noexcept { return final_density_; }

    private:
        static const nlohmann::json* find_preset(const nlohmann::json& root,
            std::string_view id) {
            if (!root.contains("data") || !root["data"].is_array()) {
                return nullptr;
            }
            for (const auto& entry : root["data"]) {
                if (entry.value("id", std::string()) == id && entry.contains("data")) {
                    return &entry["data"];
                }
            }
            return nullptr;
        }

        // The tests and the server both run with an input directory somewhere above the
        // working directory, and an embedded build answers without a filesystem at all.
        static nlohmann::json read_json(const std::string& relative) {
            std::error_code ec;
            for (const char* prefix : { "input/", "", "../input/", "../../input/" }) {
                const std::filesystem::path path = std::string(prefix) + relative;
                if (!std::filesystem::is_regular_file(path, ec) || ec) {
                    ec.clear();
                    continue;
                }
                std::ifstream file(path, std::ios::binary);
                if (!file) {
                    continue;
                }
                std::string text((std::istreambuf_iterator<char>(file)),
                    std::istreambuf_iterator<char>());
                if (text.empty()) {
                    continue;
                }
                try {
                    return nlohmann::json::parse(text);
                }
                catch (const std::exception& error) {
                    LOG_WARN(Chunk, "worldgen: cannot parse {}: {}", path.string(),
                        error.what());
                    return nlohmann::json{};
                }
            }
            // Embedded builds read from the table compiled into the binary.
            std::string text;
            if (data::input::read_text(relative, text)) {
                try {
                    return nlohmann::json::parse(text);
                }
                catch (const std::exception& error) {
                    LOG_WARN(Chunk, "worldgen: cannot parse {}: {}", relative, error.what());
                }
            }
            LOG_WARN(Chunk, "worldgen: cannot find {}", relative);
            return nlohmann::json{};
        }

        DensityGraph density_;
        BiomeParameters climate_;
        nlohmann::json material_rules_;
        nlohmann::json material_conditions_;
        std::unordered_map<std::string, DfPtr> router_;
        DfPtr final_density_;
        std::string default_block_ = "minecraft:stone";
        std::string default_fluid_ = "minecraft:water";
        std::string material_rule_id_ = "minecraft:overworld";
        std::uint32_t seed_ = 0;
        int min_y_ = -64;
        int height_ = 384;
        int sea_level_ = 63;
        bool ready_ = false;
        bool climate_ready_ = false;
    };

} // namespace terila::world::worldgen
