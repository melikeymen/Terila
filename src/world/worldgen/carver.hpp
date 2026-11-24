#pragma once

// The tunnels under the surface. A carver is not a shape painted from the noise: it is a start
// probability, a start height and a set of ellipsoids grown along a wandering line, seeded from
// the chunk position. The game walks the chunk lattice around the one being generated, gives
// every chunk a chance to open a cave, and collects the result in one mask per chunk, which is
// applied afterwards, so a tunnel opens into whatever the aquifer says stands in it.

#include "world/worldgen/density/noise.hpp"
#include "world/worldgen/java_random.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace terila::world::worldgen {

    // The circle the game's float sines come from: 65536 steps, evaluated where it is needed
    // instead of stored.
    inline float float_sin(float value) {
        const int index = static_cast<int>(value * 10430.378f) & 65535;
        return static_cast<float>(std::sin(static_cast<double>(index) * 9.5873799e-5));
    }

    inline float float_cos(double value) {
        return float_sin(static_cast<float>(value + 1.5707963267948966));
    }

    // A float a carver draws for one of its numbers: a single value, a flat range, or a range
    // with a plateau in the middle.
    struct FloatProvider {
        enum class Kind { Constant, Uniform, Trapezoid };

        Kind kind = Kind::Constant;
        float min = 0.0f;
        float max = 0.0f;
        float plateau = 0.0f;

        template <typename Random>
        float sample(Random& random) const {
            switch (kind) {
            case Kind::Uniform:
                return min + random.next_float() * (max - min);
            case Kind::Trapezoid: {
                const float span = max - min;
                const float slope = (span - plateau) / 2.0f;
                const float flat = span - slope;
                return min + random.next_float() * flat + random.next_float() * slope;
            }
            case Kind::Constant:
            default:
                return min;
            }
        }

        static FloatProvider parse(const nlohmann::json& value, float fallback) {
            FloatProvider result;
            result.min = fallback;
            result.max = fallback;
            if (value.is_number()) {
                result.min = value.get<float>();
                result.max = result.min;
                return result;
            }
            if (!value.is_object()) {
                return result;
            }
            const std::string type = value.value("type", std::string("minecraft:constant"));
            result.min = value.value("min", result.min);
            result.max = value.value("max", result.max);
            if (type.find("trapezoid") != std::string::npos) {
                result.kind = Kind::Trapezoid;
                result.plateau = value.value("plateau", 0.0f);
            } else if (type.find("uniform") != std::string::npos) {
                result.kind = Kind::Uniform;
                result.min = value.value("min_inclusive", result.min);
                result.max = value.value("max_inclusive", result.max);
            } else {
                result.kind = Kind::Constant;
                result.max = result.min;
            }
            return result;
        }
    };

    // How many caves a start chunk opens.
    struct IntProvider {
        enum class Kind { Constant, Uniform, BiasedToBottom, VeryBiasedToBottom };

        Kind kind = Kind::Constant;
        int min = 0;
        int max = 0;

        template <typename Random>
        int sample(Random& random) const {
            const int span = max - min + 1;
            switch (kind) {
            case Kind::Uniform:
                return min + random.next_int(span);
            case Kind::BiasedToBottom:
                return min + random.next_int(random.next_int(span) + 1);
            case Kind::VeryBiasedToBottom:
                return min + random.next_int(random.next_int(random.next_int(span) + 1) + 1);
            case Kind::Constant:
            default:
                return min;
            }
        }

        static IntProvider parse(const nlohmann::json& value) {
            IntProvider result;
            if (value.is_number()) {
                result.min = value.get<int>();
                result.max = result.min;
                return result;
            }
            if (!value.is_object()) {
                return result;
            }
            const std::string type = value.value("type", std::string("minecraft:constant"));
            result.min = value.value("min_inclusive", value.value("value", 0));
            result.max = value.value("max_inclusive", result.min);
            if (type.find("very_biased_to_bottom") != std::string::npos) {
                result.kind = Kind::VeryBiasedToBottom;
            } else if (type.find("biased_to_bottom") != std::string::npos) {
                result.kind = Kind::BiasedToBottom;
            } else if (type.find("uniform") != std::string::npos) {
                result.kind = Kind::Uniform;
            }
            return result;
        }
    };

    // Where a start height is counted from.
    struct HeightAnchor {
        enum class Kind { Absolute, AboveBottom, BelowTop };

        Kind kind = Kind::Absolute;
        int value = 0;

        int resolve(int min_y, int world_height) const {
            switch (kind) {
            case Kind::AboveBottom:
                return min_y + value;
            case Kind::BelowTop:
                return min_y + world_height - 1 - value;
            case Kind::Absolute:
            default:
                return value;
            }
        }

        static HeightAnchor parse(const nlohmann::json& value) {
            HeightAnchor result;
            if (value.is_number()) {
                result.value = value.get<int>();
                return result;
            }
            if (!value.is_object()) {
                return result;
            }
            if (value.contains("above_bottom")) {
                result.kind = Kind::AboveBottom;
                result.value = value["above_bottom"].get<int>();
            } else if (value.contains("below_top")) {
                result.kind = Kind::BelowTop;
                result.value = value["below_top"].get<int>();
            } else {
                result.value = value.value("absolute", 0);
            }
            return result;
        }
    };

    struct HeightProvider {
        enum class Kind { Constant, Uniform };

        Kind kind = Kind::Constant;
        HeightAnchor min;
        HeightAnchor max;

        int sample(LegacyRandom& random, int min_y, int world_height) const {
            const int low = min.resolve(min_y, world_height);
            if (kind == Kind::Constant) {
                return low;
            }
            const int high = max.resolve(min_y, world_height);
            if (low > high) {
                return low;
            }
            return low + random.next_int(high - low + 1);
        }

        static HeightProvider parse(const nlohmann::json& value) {
            HeightProvider result;
            if (!value.is_object()) {
                result.min = HeightAnchor::parse(value);
                result.max = result.min;
                return result;
            }
            const std::string type = value.value("type", std::string("minecraft:constant"));
            result.min = HeightAnchor::parse(value.contains("min_inclusive")
                ? value["min_inclusive"] : value.at("value"));
            result.max = HeightAnchor::parse(value.contains("max_inclusive")
                ? value["max_inclusive"] : value.at("value"));
            if (type.find("uniform") != std::string::npos) {
                result.kind = Kind::Uniform;
            }
            return result;
        }
    };

    // One configured carver, as the registry spells it out.
    struct ConfiguredCarver {
        enum class Kind { Cave, Canyon };

        Kind kind = Kind::Cave;
        std::string id;
        float probability = 0.0f;
        HeightProvider height;

        // cave
        IntProvider count;
        FloatProvider thickness;
        bool weird_thickness_bias = false;
        FloatProvider room_vertical_radius_multiplier;
        FloatProvider horizontal_radius_multiplier;
        FloatProvider vertical_radius_multiplier;
        FloatProvider start_vertical_radius_multiplier;
        FloatProvider floor_level;

        // canyon
        FloatProvider vertical_rotation;
        FloatProvider distance_factor;
        FloatProvider horizontal_radius_factor;
        FloatProvider canyon_y_scale;
        int width_smoothness = 3;
        float vertical_radius_default_factor = 1.0f;
        float vertical_radius_center_factor = 0.0f;

        float draw_thickness(LegacyRandom& random) const {
            float value = thickness.sample(random);
            if (weird_thickness_bias && random.next_int(10) == 0) {
                value *= random.next_float() * random.next_float() * 3.0f + 1.0f;
            }
            return value;
        }

        static ConfiguredCarver parse(std::string id_in, const nlohmann::json& data) {
            ConfiguredCarver carver;
            carver.id = std::move(id_in);
            carver.probability = data.value("probability", 0.0f);
            carver.height = HeightProvider::parse(data.at("y"));
            const std::string type = data.value("type", std::string("minecraft:cave"));
            if (type.find("canyon") != std::string::npos) {
                carver.kind = Kind::Canyon;
                carver.vertical_rotation = FloatProvider::parse(data.at("vertical_rotation"), 0.0f);
                const nlohmann::json& shape = data.at("shape");
                carver.distance_factor = FloatProvider::parse(shape.at("distance_factor"), 1.0f);
                carver.thickness = FloatProvider::parse(shape.at("thickness"), 0.0f);
                carver.width_smoothness = shape.value("width_smoothness", 3);
                carver.horizontal_radius_factor =
                    FloatProvider::parse(shape.at("horizontal_radius_factor"), 1.0f);
                carver.vertical_radius_default_factor =
                    shape.value("vertical_radius_default_factor", 1.0f);
                carver.vertical_radius_center_factor =
                    shape.value("vertical_radius_center_factor", 0.0f);
                carver.canyon_y_scale = FloatProvider::parse(shape.at("y_scale"), 1.0f);
                return carver;
            }
            carver.count = IntProvider::parse(data.at("count"));
            carver.thickness = FloatProvider::parse(data.at("thickness"), 0.0f);
            carver.weird_thickness_bias = data.value("weird_thickness_bias", false);
            carver.room_vertical_radius_multiplier =
                FloatProvider::parse(data.at("room_vertical_radius_multiplier"), 1.0f);
            carver.horizontal_radius_multiplier =
                FloatProvider::parse(data.at("horizontal_radius_multiplier"), 1.0f);
            carver.vertical_radius_multiplier =
                FloatProvider::parse(data.at("vertical_radius_multiplier"), 1.0f);
            carver.start_vertical_radius_multiplier = FloatProvider::parse(
                data.contains("start_vertical_radius_multiplier")
                    ? data["start_vertical_radius_multiplier"] : nlohmann::json(), 1.0f);
            carver.floor_level = FloatProvider::parse(data.at("floor_level"), 0.0f);
            return carver;
        }
    };

    // The blocks one chunk loses to the carvers, before anything is opened: every set cell is
    // refilled from the aquifer once the mask is applied.
    class CarvingMask {
    public:
        void begin(int min_y, int max_y) {
            min_y_ = min_y;
            max_y_ = max_y;
            const std::size_t cells = static_cast<std::size_t>(max_y - min_y + 1) * 256;
            if (cells_.size() < cells) {
                cells_.resize(cells);
            }
            std::fill(cells_.begin(), cells_.begin() + cells, 0);
            carved_ = false;
            count_ = 0;
        }

        void carve(int x, int y, int z) {
            if (x < 0 || x > 15 || z < 0 || z > 15 || y < min_y_ || y > max_y_) {
                return;
            }
            std::uint8_t& cell = cells_[index(x, y, z)];
            if (cell == 0) {
                cell = 1;
                ++count_;
            }
            carved_ = true;
        }

        bool contains(int x, int y, int z) const {
            if (x < 0 || x > 15 || z < 0 || z > 15 || y < min_y_ || y > max_y_) {
                return false;
            }
            return cells_[index(x, y, z)] != 0;
        }

        bool empty() const { return !carved_; }
        int min_y() const { return min_y_; }
        int max_y() const { return max_y_; }
        int carved_cells() const { return count_; }

    private:
        std::size_t index(int x, int y, int z) const {
            return (static_cast<std::size_t>(y - min_y_) * 16 + static_cast<std::size_t>(z)) * 16 +
                static_cast<std::size_t>(x);
        }

        std::vector<std::uint8_t> cells_;
        int min_y_ = 0;
        int max_y_ = 0;
        bool carved_ = false;
        int count_ = 0;
    };

    namespace detail {

        // The mask of one chunk, filled by the shapes of one configured carver.
        class CarverShapes {
        public:
            // The shapes are placed from the source chunk's coordinates but kept only inside
            // the chunk being generated, so both are carried here.
            CarverShapes(const ConfiguredCarver& carver, CarvingMask& mask, int origin_x,
                int origin_z, int source_x, int source_z, int min_gen_y, int gen_depth)
                : carver_(carver), mask_(mask), origin_x_(origin_x), origin_z_(origin_z),
                  source_x_(source_x), source_z_(source_z), min_gen_y_(min_gen_y),
                  gen_depth_(gen_depth) {
            }

            void carve_cave(LegacyRandom& random) {
                constexpr int kMaxReach = (4 * 2 - 1) * 16;
                const int caves = carver_.count.sample(random);
                for (int cave = 0; cave < caves; ++cave) {
                    const double start_x = static_cast<double>(source_x_ * 16 +
                        random.next_int(16));
                    const double start_y = static_cast<double>(carver_.height.sample(random,
                        min_gen_y_, gen_depth_));
                    const double start_z = static_cast<double>(source_z_ * 16 +
                        random.next_int(16));
                    const double horizontal = carver_.horizontal_radius_multiplier.sample(random);
                    const double vertical = carver_.vertical_radius_multiplier.sample(random);
                    const double start_vertical =
                        carver_.start_vertical_radius_multiplier.sample(random);
                    const double floor_level = carver_.floor_level.sample(random);
                    int tunnels = 1;
                    if (random.next_int(4) == 0) {
                        const double room_scale =
                            carver_.room_vertical_radius_multiplier.sample(random);
                        const float thickness = 1.0f + random.next_float() * 6.0f;
                        room(start_x, start_y, start_z, thickness, room_scale, floor_level);
                        tunnels += random.next_int(4);
                    }
                    for (int tunnel = 0; tunnel < tunnels; ++tunnel) {
                        const float yaw = random.next_float() * 6.2831855f;
                        const float pitch = (random.next_float() - 0.5f) / 4.0f;
                        const float thickness = carver_.draw_thickness(random);
                        const int distance = kMaxReach - random.next_int(kMaxReach / 4);
                        tunnel_path(random.next_long(), start_x, start_y, start_z, horizontal,
                            vertical, thickness, yaw, pitch, 0, distance, start_vertical,
                            floor_level);
                    }
                }
            }

            void carve_canyon(LegacyRandom& random) {
                constexpr int kMaxReach = (4 * 2 - 1) * 16;
                const double start_x = static_cast<double>(source_x_ * 16 + random.next_int(16));
                const int start_y = carver_.height.sample(random, min_gen_y_, gen_depth_);
                const double start_z = static_cast<double>(source_z_ * 16 + random.next_int(16));
                const float yaw = random.next_float() * 6.2831855f;
                const float pitch = carver_.vertical_rotation.sample(random);
                const double y_scale = carver_.canyon_y_scale.sample(random);
                const float thickness = carver_.thickness.sample(random);
                const int distance = static_cast<int>(static_cast<float>(kMaxReach) *
                    carver_.distance_factor.sample(random));
                canyon_path(random.next_long(), start_x, static_cast<double>(start_y), start_z,
                    thickness, yaw, pitch, 0, distance, y_scale);
            }

        private:
            // An ellipsoid at (x, y, z), kept only where the carver's own test lets it through.
            template <typename Skip>
            void ellipsoid(double x, double y, double z, double horizontal_radius,
                double vertical_radius, Skip skip) {
                const double middle_x = static_cast<double>(origin_x_ * 16 + 8);
                const double middle_z = static_cast<double>(origin_z_ * 16 + 8);
                const double reach = 16.0 + horizontal_radius * 2.0;
                if (std::abs(x - middle_x) > reach || std::abs(z - middle_z) > reach) {
                    return;
                }
                const int base_x = origin_x_ * 16;
                const int base_z = origin_z_ * 16;
                const int from_x = std::max(
                    static_cast<int>(std::floor(x - horizontal_radius)) - base_x - 1, 0);
                const int to_x = std::min(
                    static_cast<int>(std::floor(x + horizontal_radius)) - base_x, 15);
                const int from_y = std::max(
                    static_cast<int>(std::floor(y - vertical_radius)) - 1, mask_.min_y());
                const int to_y = std::min(
                    static_cast<int>(std::floor(y + vertical_radius)) + 1, mask_.max_y());
                const int from_z = std::max(
                    static_cast<int>(std::floor(z - horizontal_radius)) - base_z - 1, 0);
                const int to_z = std::min(
                    static_cast<int>(std::floor(z + horizontal_radius)) - base_z, 15);

                for (int x_index = from_x; x_index <= to_x; ++x_index) {
                    const double xd = static_cast<double>(base_x + x_index) + 0.5 - x;
                    const double nx = xd / horizontal_radius;
                    for (int z_index = from_z; z_index <= to_z; ++z_index) {
                        const double zd = static_cast<double>(base_z + z_index) + 0.5 - z;
                        const double nz = zd / horizontal_radius;
                        if (nx * nx + nz * nz >= 1.0) {
                            continue;
                        }
                        for (int block_y = to_y; block_y > from_y; --block_y) {
                            const double ny = (static_cast<double>(block_y) - 0.5 - y) /
                                vertical_radius;
                            if (skip(nx, ny, nz, block_y)) {
                                continue;
                            }
                            mask_.carve(x_index, block_y, z_index);
                        }
                    }
                }
            }

            bool cave_skip(double xd, double yd, double zd, double floor_level) const {
                return yd <= floor_level || xd * xd + yd * yd + zd * zd >= 1.0;
            }

            void room(double x, double y, double z, float thickness, double y_scale,
                double floor_level) {
                const double horizontal = 1.5 + static_cast<double>(float_sin(1.5707964f) *
                    thickness);
                const double vertical = horizontal * y_scale;
                ellipsoid(x + 1.0, y, z, horizontal, vertical,
                    [this, floor_level](double xd, double yd, double zd, int) {
                        return cave_skip(xd, yd, zd, floor_level);
                    });
            }

            void tunnel_path(std::uint64_t seed, double x, double y, double z,
                double horizontal_scale, double vertical_scale, float thickness, float yaw,
                float pitch, int step, int distance, double start_vertical_scale,
                double floor_level) {
                // A tunnel keeps its own stream, so the branch it grows does not shift the
                // numbers the rest of the path draws.
                RandomSource random = tunnel_random(seed);
                const int split = random.next_int(distance / 2) + distance / 4;
                const bool steep = random.next_int(6) == 0;
                float pitch_drift = 0.0f;
                float yaw_drift = 0.0f;

                for (int walked = step; walked < distance; ++walked) {
                    const double horizontal = 1.5 + static_cast<double>(float_sin(
                        static_cast<float>(3.1415927f * static_cast<float>(walked) /
                            static_cast<float>(distance))) * thickness);
                    const double vertical = horizontal * start_vertical_scale;
                    const float lean = float_cos(static_cast<double>(pitch));
                    x += static_cast<double>(float_cos(static_cast<double>(yaw)) * lean);
                    y += static_cast<double>(float_sin(pitch));
                    z += static_cast<double>(float_sin(static_cast<double>(yaw)) * lean);
                    pitch *= steep ? 0.92f : 0.7f;
                    pitch += pitch_drift * 0.1f;
                    yaw += yaw_drift * 0.1f;
                    pitch_drift *= 0.9f;
                    yaw_drift *= 0.75f;
                    pitch_drift += (random_float(random) - random_float(random)) *
                        random_float(random) * 2.0f;
                    yaw_drift += (random_float(random) - random_float(random)) *
                        random_float(random) * 4.0f;

                    if (walked == split && thickness > 1.0f) {
                        tunnel_path(random.next_long(), x, y, z, horizontal_scale,
                            vertical_scale, random_float(random) * 0.5f + 0.5f,
                            yaw - 1.5707964f, pitch / 3.0f, walked, distance, 1.0, floor_level);
                        tunnel_path(random.next_long(), x, y, z, horizontal_scale,
                            vertical_scale, random_float(random) * 0.5f + 0.5f,
                            yaw + 1.5707964f, pitch / 3.0f, walked, distance, 1.0, floor_level);
                        return;
                    }
                    if (random.next_int(4) != 0) {
                        if (!reaches(x, z, walked, distance, thickness)) {
                            return;
                        }
                        ellipsoid(x, y, z, horizontal * horizontal_scale,
                            vertical * vertical_scale,
                            [this, floor_level](double xd, double yd, double zd, int) {
                                return cave_skip(xd, yd, zd, floor_level);
                            });
                    }
                }
            }

            bool reaches(double x, double z, int walked, int distance, float thickness) const {
                const double middle_x = static_cast<double>(origin_x_ * 16 + 8);
                const double middle_z = static_cast<double>(origin_z_ * 16 + 8);
                const double left = static_cast<double>(distance - walked);
                const double reach = static_cast<double>(thickness + 2.0f + 16.0f);
                const double dx = x - middle_x;
                const double dz = z - middle_z;
                return dx * dx + dz * dz - left * left <= reach * reach;
            }

            void canyon_path(std::uint64_t seed, double x, double y, double z, float thickness,
                float yaw, float pitch, int step, int distance, double y_scale) {
                RandomSource random = tunnel_random(seed);
                std::vector<float> width_by_height(static_cast<std::size_t>(gen_depth_), 1.0f);
                float width = 1.0f;
                for (int level = 0; level < gen_depth_; ++level) {
                    if (level == 0 || random.next_int(carver_.width_smoothness) == 0) {
                        width = 1.0f + random_float(random) * random_float(random);
                    }
                    width_by_height[static_cast<std::size_t>(level)] = width * width;
                }
                float pitch_drift = 0.0f;
                float yaw_drift = 0.0f;

                for (int walked = step; walked < distance; ++walked) {
                    double horizontal = 1.5 + static_cast<double>(float_sin(
                        static_cast<float>(static_cast<float>(walked) * 3.1415927f /
                            static_cast<float>(distance))) * thickness);
                    double vertical = horizontal * y_scale;
                    horizontal *= static_cast<double>(carver_.horizontal_radius_factor
                        .sample(random));
                    const float middle = 1.0f - std::abs(0.5f - static_cast<float>(walked) /
                        static_cast<float>(distance)) * 2.0f;
                    const float factor = carver_.vertical_radius_default_factor +
                        carver_.vertical_radius_center_factor * middle;
                    vertical = static_cast<double>(factor) * vertical *
                        (0.75 + random_float(random) * 0.25);
                    const float lean = float_cos(static_cast<double>(pitch));
                    const float drop = float_sin(pitch);
                    x += static_cast<double>(float_cos(static_cast<double>(yaw)) * lean);
                    y += static_cast<double>(drop);
                    z += static_cast<double>(float_sin(static_cast<double>(yaw)) * lean);
                    pitch *= 0.7f;
                    pitch += pitch_drift * 0.05f;
                    yaw += yaw_drift * 0.05f;
                    pitch_drift *= 0.8f;
                    yaw_drift *= 0.5f;
                    pitch_drift += (random_float(random) - random_float(random)) *
                        random_float(random) * 2.0f;
                    yaw_drift += (random_float(random) - random_float(random)) *
                        random_float(random) * 4.0f;

                    if (random.next_int(4) != 0) {
                        if (!reaches(x, z, walked, distance, thickness)) {
                            return;
                        }
                        ellipsoid(x, y, z, horizontal, vertical,
                            [this, &width_by_height](double xd, double yd, double zd,
                                int block_y) {
                                const std::size_t level =
                                    static_cast<std::size_t>(block_y - min_gen_y_);
                                const float narrowed = level > 0
                                    ? width_by_height[level - 1]
                                    : width_by_height[0];
                                return (xd * xd + zd * zd) * static_cast<double>(narrowed) +
                                    yd * yd / 6.0 >= 1.0;
                            });
                    }
                }
            }

            static float random_float(RandomSource& random) {
                return static_cast<float>(random.next_long() >> 40) * 5.9604645e-8f;
            }

            // The stream a tunnel walks on: the game widens this seed into the two halves the
            // generator runs on, without stirring it through the noise seed mixer.
            static RandomSource tunnel_random(std::uint64_t seed) {
                const std::uint64_t low = seed ^ 0x6A09E667F3BCC909ull;
                return RandomSource(low, low + 0x9E3779B97F4A7C15ull);
            }

            const ConfiguredCarver& carver_;
            CarvingMask& mask_;
            int origin_x_ = 0;
            int origin_z_ = 0;
            int source_x_ = 0;
            int source_z_ = 0;
            int min_gen_y_ = 0;
            int gen_depth_ = 0;
        };

    } // namespace detail

    // The configured carvers, indexed the way each biome lists them.
    class Carvers {
    public:
        bool load(const nlohmann::json& configured, const nlohmann::json& biomes,
            const std::function<int(std::string_view)>& biome_index_of) {
            if (!configured.contains("registries") ||
                !configured["registries"].contains("entries")) {
                return false;
            }
            std::unordered_map<std::string, int> positions;
            for (const auto& entry : configured["registries"]["entries"]) {
                if (!entry.contains("id") || !entry.contains("data")) {
                    continue;
                }
                ConfiguredCarver carver;
                try {
                    carver = ConfiguredCarver::parse(entry["id"].get<std::string>(),
                        entry["data"]);
                } catch (const std::exception&) {
                    // A carver shape this build does not understand is skipped, and the biome
                    // lists below simply never name it.
                    continue;
                }
                positions[carver.id] = static_cast<int>(configured_.size());
                configured_.push_back(std::move(carver));
            }
            if (configured_.empty()) {
                return false;
            }

            // Every biome names its carvers in order; the order is the seed offset, so it is
            // kept exactly as the data spells it.
            per_biome_.assign(64, {});
            default_list_ = {};
            if (biomes.contains("data") && biomes["data"].is_array()) {
                for (const auto& entry : biomes["data"]) {
                    if (!entry.contains("id") || !entry.contains("data")) {
                        continue;
                    }
                    const auto& data = entry["data"];
                    if (!data.contains("carvers") || !data["carvers"].is_array()) {
                        continue;
                    }
                    std::vector<int> list;
                    for (const auto& name : data["carvers"]) {
                        if (!name.is_string()) {
                            continue;
                        }
                        const auto found = positions.find(name.get<std::string>());
                        if (found != positions.end()) {
                            list.push_back(found->second);
                        }
                    }
                    const int index = biome_index_of(entry["id"].get<std::string>());
                    if (index >= 0 && index < static_cast<int>(per_biome_.size())) {
                        per_biome_[static_cast<std::size_t>(index)] = list;
                    }
                    if (default_list_.empty() && !list.empty()) {
                        default_list_ = list;
                    }
                }
            }
            return !default_list_.empty();
        }

        bool empty() const { return configured_.empty(); }
        int size() const { return static_cast<int>(configured_.size()); }
        const ConfiguredCarver& at(int index) const { return configured_[static_cast<std::size_t>(index)]; }

        const std::vector<int>& list_for_biome(int biome) const {
            if (biome < 0 || biome >= static_cast<int>(per_biome_.size())) {
                return default_list_;
            }
            const std::vector<int>& list = per_biome_[static_cast<std::size_t>(biome)];
            return list.empty() ? default_list_ : list;
        }

        bool starts(int index, LegacyRandom& random) const {
            return random.next_float() <= configured_[static_cast<std::size_t>(index)].probability;
        }

        // One start chunk's shapes, kept inside the chunk being generated.
        static void carve(const ConfiguredCarver& carver, CarvingMask& mask, int chunk_x,
            int chunk_z, int source_x, int source_z, int min_gen_y, int gen_depth,
            LegacyRandom& random) {
            detail::CarverShapes shapes(carver, mask, chunk_x, chunk_z, source_x, source_z,
                min_gen_y, gen_depth);
            if (carver.kind == ConfiguredCarver::Kind::Cave) {
                shapes.carve_cave(random);
            } else {
                shapes.carve_canyon(random);
            }
        }

    private:
        std::vector<ConfiguredCarver> configured_;
        std::vector<std::vector<int>> per_biome_;
        std::vector<int> default_list_;
    };

} // namespace terila::world::worldgen
