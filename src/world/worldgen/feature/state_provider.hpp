#pragma once

// What a placed feature asks for the block it puts down, read from the game's provider data.
// A patch of flowers names a provider instead of a block, and the provider owns a noise field
// seeded from the data: the species follows the field at the block, so a meadow holds drifts of
// one flower and a plains keeps its tulips in one cold corner instead of scattering all of them
// everywhere. The weighted form is the same list without a field, rolled once per block. Only
// plants that stand in a single cell are kept; the lower or upper half of a taller plant is not
// something this planter can stand up, so a provider left with no state is empty.

#include "world/terrain_biomes.hpp"
#include "world/terrain_noise.hpp"
#include "world/worldgen/density/noise.hpp"
#include "log/logger.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace terila::world::worldgen {

    // The providers of the data pack, by the id they are registered under.
    using ProviderRegistry = std::unordered_map<std::string, const nlohmann::json*>;

    class StateProvider {
    public:
        using Json = nlohmann::json;

        // Reads the provider a `to_place` value names. A string is a registry provider when the
        // registry knows it and a block otherwise, so a feature that spells its block directly
        // and one that hands the choice to a provider are the same call.
        bool build(const Json& value, const terrain::IdResolver& block,
            const ProviderRegistry& registry, int depth = 0) {
            reset();
            if (depth > 6) {
                return false;
            }
            if (value.is_string()) {
                const std::string name = value.get<std::string>();
                if (const auto found = registry.find(name); found != registry.end()) {
                    return build(*found->second, block, registry, depth + 1);
                }
                const auto state = add_block(name, block);
                if (!state) {
                    return false;
                }
                kind_ = Kind::Weighted;
                entries_.push_back({ *state, 1.0 });
                return true;
            }
            if (!value.is_object()) {
                return false;
            }

            const std::string type = value.value("type", std::string());
            if (type == "minecraft:noise" || type == "minecraft:noise_threshold" ||
                type == "minecraft:dual_noise") {
                return build_noise(type, value, block);
            }
            if (type == "minecraft:weighted" || type == "minecraft:simple_state_provider" ||
                type == "minecraft:randomized_int_state_provider") {
                return build_list(value, block, registry, depth);
            }

            // A bare state: the data spells a single block as its id with a map of properties.
            const auto state = add_block(value, block);
            if (!state) {
                return false;
            }
            kind_ = Kind::Weighted;
            entries_.push_back({ *state, 1.0 });
            return true;
        }

        bool empty() const noexcept {
            if (kind_ == Kind::None) {
                return true;
            }
            // A threshold provider keeps its default state when one of its two lists held
            // only plants this checkout cannot stand up: the field still decides which half
            // of the palette is in play, the missing half falls back to the default.
            if (kind_ == Kind::Threshold) {
                return default_ == 0 && low_.empty() && high_.empty();
            }
            return entries_.empty();
        }

        // The block a patch would put down at a column. The salt keeps two providers of one
        // column from rolling the same number.
        std::int32_t state_at(std::int32_t x, std::int32_t y, std::int32_t z,
            std::uint32_t salt) const {
            if (empty()) {
                return 0;
            }
            switch (kind_) {
            case Kind::Weighted:
                return weighted_at(x, z, salt);
            case Kind::Noise:
                return pick(entries_, sample(x, y, z, scale_, noise_));
            case Kind::Threshold: {
                const float value = sample(x, y, z, scale_, noise_);
                if (static_cast<double>(value) < static_cast<double>(threshold_)) {
                    return low_.empty() ? default_ : choose(low_, x, z, salt ^ 0x51ED270Bu);
                }
                if (high_.empty() || roll(x, z, salt) >= high_chance_) {
                    return default_;
                }
                return choose(high_, x, z, salt ^ 0x2F9B1C7Du);
            }
            case Kind::Dual:
                return dual_at(x, y, z);
            case Kind::None:
                break;
            }
            return 0;
        }

        // The state a biome planter falls back to when the provider cannot be built; it is the
        // first state the data lists, not a block chosen by this build.
        std::int32_t first_state() const {
            if (kind_ == Kind::Threshold) {
                return default_;
            }
            return entries_.empty() ? std::int32_t(0) : entries_.front().state;
        }

        // Every block the provider may put down, as the data names it, for the caller that has
        // to decide which kind of patch this is.
        const std::vector<std::string>& names() const noexcept { return names_; }

    private:
        enum class Kind : std::uint8_t { None, Weighted, Noise, Threshold, Dual };

        struct Entry {
            std::int32_t state = 0;
            double weight = 1.0;

            Entry() = default;
            Entry(std::int32_t state_, double weight_) : state(state_), weight(weight_) {
            }
        };

        void reset() {
            kind_ = Kind::None;
            entries_.clear();
            low_.clear();
            high_.clear();
            names_.clear();
            default_ = 0;
            threshold_ = 0.0f;
            high_chance_ = 0.0f;
            scale_ = 1.0f;
            slow_scale_ = 1.0f;
            variety_low_ = 1;
            variety_high_ = 1;
        }

        bool build_list(const Json& value, const terrain::IdResolver& block,
            const ProviderRegistry& registry, int depth) {
            const std::string type = value.value("type", std::string());
            if (type == "minecraft:simple_state_provider") {
                const auto state = add_block(value.value("state", Json()), block);
                if (!state) {
                    return false;
                }
                kind_ = Kind::Weighted;
                entries_.push_back({ *state, 1.0 });
                return true;
            }
            if (type == "minecraft:randomized_int_state_provider") {
                return value.contains("source")
                    ? build(value["source"], block, registry, depth + 1) : false;
            }

            const Json* list = nullptr;
            if (value.contains("entries") && value["entries"].is_array()) {
                list = &value["entries"];
            }
            else if (value.is_array()) {
                list = &value;
            }
            if (list == nullptr) {
                return false;
            }
            kind_ = Kind::Weighted;
            for (const auto& entry : *list) {
                const Json& data = entry.is_object() && entry.contains("data") ? entry["data"]
                    : entry;
                const double weight = entry.is_object() ? entry.value("weight", 1.0) : 1.0;
                const auto state = add_block(data, block);
                if (!state) {
                    continue;
                }
                entries_.push_back({ *state, weight });
            }
            if (entries_.empty()) {
                kind_ = Kind::None;
            }
            return !entries_.empty();
        }

        bool build_noise(const std::string& type, const Json& value,
            const terrain::IdResolver& block) {
            const std::uint64_t seed = static_cast<std::uint64_t>(
                value.value("seed", static_cast<long long>(0)));
            scale_ = value.value("scale", 1.0f);
            RandomSource random = RandomSource::from_seed(seed);

            const auto collect = [&](const Json& list, std::vector<Entry>& out) {
                if (!list.is_array()) {
                    return;
                }
                for (const auto& item : list) {
                    const auto state = add_block(item, block);
                    if (state) {
                        out.push_back({ *state, 1.0 });
                    }
                }
            };

            if (type == "minecraft:noise_threshold") {
                kind_ = Kind::Threshold;
                threshold_ = value.value("threshold", 0.0f);
                high_chance_ = value.value("high_chance", 0.0f);
                const auto fallback = add_block(value.value("default_state", Json()), block);
                if (fallback) {
                    default_ = *fallback;
                }
                collect(value.value("low_states", Json()), low_);
                collect(value.value("high_states", Json()), high_);
                if (empty()) {
                    kind_ = Kind::None;
                    return false;
                }
                spec_ = noise_spec(value.value("noise", Json()));
                noise_.init(spec_, random);
                return true;
            }

            if (type == "minecraft:dual_noise") {
                kind_ = Kind::Dual;
                const Json& variety = value.value("variety", Json());
                if (variety.is_array() && variety.size() >= 2) {
                    variety_low_ = variety[0].get<int>();
                    variety_high_ = variety[1].get<int>();
                }
                slow_scale_ = value.value("slow_scale", 1.0f);
                slow_spec_ = noise_spec(value.value("slow_noise", Json()));
                RandomSource slow_random = RandomSource::from_seed(seed);
                slow_noise_.init(slow_spec_, slow_random);
                collect(value.value("states", Json()), entries_);
                if (entries_.empty()) {
                    kind_ = Kind::None;
                    return false;
                }
                spec_ = noise_spec(value.value("noise", Json()));
                noise_.init(spec_, random);
                return true;
            }

            kind_ = Kind::Noise;
            collect(value.value("states", Json()), entries_);
            if (entries_.empty()) {
                kind_ = Kind::None;
                return false;
            }
            spec_ = noise_spec(value.value("noise", Json()));
            noise_.init(spec_, random);
            return true;
        }

        // The noise the provider fields were dumped with: one octave unless the data says more,
        // and the amplitude normalisation the registry also applies.
        static NoiseSpec noise_spec(const Json& noise) {
            NoiseSpec spec;
            if (!noise.is_object()) {
                return spec;
            }
            spec.base_octave = noise.value("base_octave", 0);
            spec.base_amplitude = noise.value("base_amplitude", 1.0);
            spec.octave_count = noise.value("octave_count", 1);
            if (noise.contains("normalize")) {
                if (noise["normalize"].is_boolean()) {
                    spec.normalize = noise["normalize"].get<bool>();
                }
                else if (noise["normalize"].is_string()) {
                    spec.normalize = noise["normalize"].get<std::string>() != "legacy";
                }
            }
            if (noise.contains("amplitude_modifiers") &&
                noise["amplitude_modifiers"].is_array()) {
                for (const auto& value : noise["amplitude_modifiers"]) {
                    if (value.is_number()) {
                        spec.amplitude_modifiers.push_back(value.get<double>());
                    }
                }
            }
            return spec;
        }

        std::optional<std::int32_t> add_block(const Json& value,
            const terrain::IdResolver& block) {
            if (value.is_string()) {
                return add_block(value.get<std::string>(), block);
            }
            if (!value.is_object()) {
                return std::nullopt;
            }
            // A taller plant is dumped as its lower and upper half; neither half stands on its
            // own, so the provider loses those states instead of planting a floating block.
            if (value.contains("properties") && value["properties"].is_object() &&
                value["properties"].contains("half")) {
                return std::nullopt;
            }
            return add_block(value.value("id", std::string()), block);
        }

        std::optional<std::int32_t> add_block(const std::string& name,
            const terrain::IdResolver& block) {
            if (name.empty()) {
                return std::nullopt;
            }
            const auto state = block(name);
            if (!state) {
                return std::nullopt;
            }
            if (std::find(names_.begin(), names_.end(), name) == names_.end()) {
                names_.push_back(name);
            }
            return state;
        }

        std::int32_t weighted_at(std::int32_t x, std::int32_t z, std::uint32_t salt) const {
            double total = 0.0;
            for (const auto& entry : entries_) {
                total += std::max(0.0, entry.weight);
            }
            if (total <= 0.0) {
                return entries_.front().state;
            }
            const double unit = static_cast<double>(terrain::detail::hash3(x, z, salt) % 100000u) /
                100000.0;
            double left = unit * total;
            for (const auto& entry : entries_) {
                left -= std::max(0.0, entry.weight);
                if (left < 0.0) {
                    return entry.state;
                }
            }
            return entries_.back().state;
        }

        // A field that decides how much of the palette is in play at this spot, then the field
        // that picks inside it. The slow field is sampled at three offsets so the short list of
        // one spot is not the list of its neighbour.
        std::int32_t dual_at(std::int32_t x, std::int32_t y, std::int32_t z) const {
            const float slow = sample(x, y, z, slow_scale_, slow_noise_);
            const float clamped = std::clamp(slow, -1.0f, 1.0f);
            const float mapped = static_cast<float>(variety_low_) + (clamped + 1.0f) * 0.5f *
                static_cast<float>(variety_high_ + 1 - variety_low_);
            const int variety = std::clamp(static_cast<int>(mapped), 1, 32);

            std::array<std::int32_t, 32> possible{};
            int count = 0;
            for (int i = 0; i < variety; ++i) {
                const std::int32_t state = pick(entries_,
                    sample(x + i * 54545, y, z + i * 34234, slow_scale_, slow_noise_));
                if (state != 0) {
                    possible[static_cast<std::size_t>(count)] = state;
                    ++count;
                }
            }
            if (count == 0) {
                return 0;
            }
            const float value = sample(x, y, z, scale_, noise_);
            const float placed = std::clamp((1.0f + value) / 2.0f, 0.0f, 0.9999f);
            const auto index = std::min(
                static_cast<std::size_t>(placed * static_cast<float>(count)),
                static_cast<std::size_t>(count - 1));
            return possible[index];
        }

        static std::int32_t pick(const std::vector<Entry>& choices, float value) {
            if (choices.empty()) {
                return 0;
            }
            const float placed = std::clamp((1.0f + value) / 2.0f, 0.0f, 0.9999f);
            const auto index = std::min(
                static_cast<std::size_t>(placed * static_cast<float>(choices.size())),
                choices.size() - 1);
            return choices[index].state;
        }

        static std::int32_t choose(const std::vector<Entry>& choices, std::int32_t x,
            std::int32_t z, std::uint32_t salt) {
            if (choices.empty()) {
                return 0;
            }
            const auto index = terrain::detail::hash3(x, z, salt) % choices.size();
            return choices[index].state;
        }

        // The field is sampled at the block scaled by the provider's own scale, in single
        // precision, the way the data intends the frequency to read.
        static float sample(std::int32_t x, std::int32_t y, std::int32_t z, float scale,
            const NormalNoise& noise) {
            const float fx = static_cast<float>(x) * scale;
            const float fy = static_cast<float>(y) * scale;
            const float fz = static_cast<float>(z) * scale;
            return noise.sample(static_cast<double>(fx), static_cast<double>(fy),
                static_cast<double>(fz));
        }

        static float roll(std::int32_t x, std::int32_t z, std::uint32_t salt) {
            return static_cast<float>(terrain::detail::hash3(x, z, salt) >> 8) /
                16777216.0f;
        }

        Kind kind_ = Kind::None;
        std::vector<Entry> entries_;
        std::vector<Entry> low_;
        std::vector<Entry> high_;
        std::vector<std::string> names_;
        std::int32_t default_ = 0;
        float threshold_ = 0.0f;
        float high_chance_ = 0.0f;
        float scale_ = 1.0f;
        float slow_scale_ = 1.0f;
        int variety_low_ = 1;
        int variety_high_ = 1;
        NoiseSpec spec_;
        NoiseSpec slow_spec_;
        NormalNoise noise_;
        NormalNoise slow_noise_;
    };

} // namespace terila::world::worldgen
