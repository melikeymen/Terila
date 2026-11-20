#pragma once

// Mobs, the way the game runs them: every tick a mob's goal selector picks the highest
// priority goal that can run (chase, panic, wander, float, tempt, breed, ...), the chosen
// goal asks for a movement speed in blocks per tick, and LivingEntity's gravity/friction/jump
// constants turn that into a position. What a mob can do, how fast it is, how hard it hits,
// how far it follows and what it eats come from the extracted tables (mob_data.hpp), not from
// this file.

#include "entity/difficulty.hpp"
#include "entity/mob_ai.hpp"
#include "entity/mob_data.hpp"
#include "log/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace terila::entity {

    struct PlayerPos {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        std::int32_t id = 0;              // entity id, so drops and hits know who it is
        std::string held_item;            // what Tempt can follow
        bool creative = false;            // monsters ignore creative players
    };

    // One landed hit. The mob manager does not know about connections, so it records who was
    // hit and how hard; the server hands the list to the players after the tick.
    struct MobHit {
        std::int32_t player = 0;
        double damage = 0.0;
        bool ranged = false;
        std::string attacker;
    };

    struct Mob {
        const data::MobSpec* spec = nullptr;

        std::int32_t id = 0;
        std::int32_t type_id = 0;   // protocol entity type of this mob, for add_entity
        std::array<std::uint8_t, 16> uuid{};
        std::string type_name;
        data::MobCategory kind = data::MobCategory::Misc;

        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        double vx = 0.0;   // current velocity, kept so friction and drag behave like vanilla
        double vy = 0.0;
        double vz = 0.0;
        bool on_ground = false;
        bool in_water = false;

        float yaw = 0.0f;
        float head_yaw = 0.0f;
        float pitch = 0.0f;

        double health = 20.0;
        double max_health = 20.0;
        float movement_speed = 0.7f;
        float attack_damage = 2.0f;
        float follow_range = 32.0f;
        float armor = 0.0f;

        bool dead = false;
        int death_ticks = 0;

        // Set for a mob that must survive distance and idle despawns: summoned, bred or
        // named. It is also what keeps a mob out of the population counts, so a farm the
        // player built does not eat the world's spawn budget.
        bool persistent = false;
        int no_action_ticks = 0;
        std::uint32_t hurt_serial = 0;
        int hurt_ticks = 0;

        // Combat.
        std::int32_t target_id = 0;
        std::int32_t attacker_id = 0;
        int attack_cooldown = 0;

        // Steering and the running goal.
        GoalKind goal = GoalKind::Unsupported;
        int goal_age = 0;
        int panic_ticks = 0;
        double wander_x = 0.0;
        double wander_z = 0.0;
        int wander_ticks = 0;
        int look_ticks = 0;
        float look_yaw = 0.0f;
        float look_pitch = 0.0f;
        int jump_ticks = 0;
        bool baby = false;
        int age_ticks = 0;

        // Breeding: 600 ticks in love after being fed, then a cooldown once a child arrives.
        int love_ticks = 0;
        int breed_cooldown = 0;

        double knockback = 0.0;
        float knockback_yaw = 0.0f;

        // The speed modifier the running goal was registered with (GoalSpec::speed).
        double goal_speed = 1.0;
        int ranged_charge = 0;
        int swell_ticks = 0;
        int hop_ticks = 0;
        double hop_x = 0.0;
        double hop_z = 0.0;
        bool in_sun = false;
        int sun_ticks = 0;

        // Derived from the species once, at spawn: the goal list and the type name never
        // change, and these were being recomputed for every mob on every tick.
        bool undead = false;
        bool needs_attack_fallback = false;
        GoalKind fallback_attack = GoalKind::Melee;

        data::MobCategory category() const noexcept { return kind; }
        bool is_monster() const noexcept { return kind == data::MobCategory::Monster; }
        bool adult() const noexcept { return !baby; }
        bool in_love() const noexcept { return love_ticks > 0; }

        // A baby is the same mob at half the size and speed; vanilla halves the bounding box
        // and the attribute is scaled by the baby flag on the entity.
        double scale() const noexcept { return baby ? 0.5 : 1.0; }
        double speed_now() const noexcept {
            const double base = static_cast<double>(movement_speed);
            return baby ? base * 0.6 : base;
        }
        bool eats(std::string_view item) const noexcept {
            return spec != nullptr && data::eats(*spec, item);
        }
    };

    class MobManager {
    public:
        using GroundResolver = std::function<double(double x, double z)>;
        using WaterResolver = std::function<bool(double x, double z)>;

        // The surface a mob stands on and whether a column is under water.
        void set_ground_resolver(GroundResolver resolver) { ground_resolver_ = std::move(resolver); }
        void set_water_resolver(WaterResolver resolver) { water_resolver_ = std::move(resolver); }

        // Whether a position has a roof over it. Undead use it to keep out of the sun; the
        // server wires it to the column's sky top.
        using ShadeResolver = std::function<bool(double x, double y, double z)>;
        void set_shade_resolver(ShadeResolver resolver) { shade_resolver_ = std::move(resolver); }
        void set_ground(double y) { ground_y_ = y; }
        void set_roam_area(double center_x, double center_z, double radius) {
            roam_x_ = center_x;
            roam_z_ = center_z;
            roam_radius_ = radius;
        }

        // The protocol's entity type ids, so a spawned mob can be described to clients.
        void set_entity_type_ids(const std::unordered_map<std::string, std::int32_t>* ids) {
            type_ids_ = ids;
        }

        // Spawns from the extracted spec: size, category, health, speed, damage, follow range
        // and the goal list all come from mob_data.hpp.
        bool spawn(const std::string& type_name, double x, double z, bool baby = false,
            bool persistent = false) {
            const auto* spec = data::find_mob_by_name(type_name);
            if (spec == nullptr) {
                LOG_WARN(Entity, "unknown entity type: {}", type_name);
                return false;
            }

            Mob mob;
            mob.spec = spec;
            mob.id = next_id_++;
            mob.type_name = std::string(spec->id);
            mob.kind = spec->category;
            mob.undead = is_undead(mob.type_name);
            mob.needs_attack_fallback = spec->category == data::MobCategory::Monster &&
                !has_attack_goal(*spec);
            mob.fallback_attack = default_attack_kind(mob.type_name);
            if (type_ids_ != nullptr) {
                if (const auto found = type_ids_->find(mob.type_name); found != type_ids_->end()) {
                    mob.type_id = found->second;
                }
            }
            mob.x = x;
            mob.z = z;
            mob.max_health = spec->max_health;
            mob.health = spec->max_health;
            mob.movement_speed = spec->movement_speed;
            mob.attack_damage = spec->attack_damage;
            mob.follow_range = spec->follow_range;
            mob.armor = spec->armor;
            mob.attack_cooldown = static_cast<int>(next_random() % kMeleeCooldownTicks);
            mob.baby = baby;
            mob.age_ticks = baby ? 0 : kBabyAge;
            mob.persistent = persistent;
            ground_at(mob);
            mob.yaw = static_cast<float>(next_random() % 360);
            mob.head_yaw = mob.yaw;

            mob.uuid[0] = 0x54;
            mob.uuid[1] = 0x65;
            mob.uuid[2] = 0x72;
            mob.uuid[3] = 0x69;
            for (int i = 4; i < 16; ++i) {
                mob.uuid[i] = static_cast<std::uint8_t>(next_random() & 0xFF);
            }
            mob.uuid[6] = static_cast<std::uint8_t>((mob.uuid[6] & 0x0F) | 0x40);
            mob.uuid[8] = static_cast<std::uint8_t>((mob.uuid[8] & 0x3F) | 0x80);

            mobs_.push_back(std::move(mob));
            id_index_[mobs_.back().id] = mobs_.size() - 1;
            const auto& spawned = mobs_.back();
            LOG_DEBUG(Entity, "spawned {} (type {} / entity {}){} at ({:.1f}, {:.1f}, {:.1f}) "
                "hp={:.0f} speed={:.2f} damage={:.1f} follow={:.0f}",
                spawned.type_name, spawned.type_id, spawned.id, spawned.baby ? " baby" : "",
                spawned.x, spawned.y, spawned.z, spawned.max_health, spawned.movement_speed,
                spawned.attack_damage, spawned.follow_range);
            return true;
        }

        // A command places the entity exactly where it asked, without snapping it to the
        // surface first: /summon names a position, not a column.
        // A position that came from a command or a save is where the entity is wanted, so it
        // is persistent unless the caller says otherwise.
        bool spawn_at(const std::string& type_name, double x, double y, double z,
            bool baby = false, bool persistent = true) {
            if (!spawn(type_name, x, z, baby, persistent)) {
                return false;
            }
            mobs_.back().y = y;
            mobs_.back().vy = 0.0;
            mobs_.back().on_ground = false;
            return true;
        }

        // Kept for callers that already hold a type id table; the id is no longer needed
        // because the spec carries everything.
        bool spawn(const std::string& type_name,
            const std::unordered_map<std::string, std::int32_t>& /*types*/,
            double x, double z, bool /*hostile*/ = false, std::int32_t /*max_health*/ = 10) {
            return spawn(type_name, x, z);
        }

        const Mob* find(std::int32_t id) const {
            const auto found = id_index_.find(id);
            if (found == id_index_.end()) {
                return nullptr;
            }
            return &mobs_[found->second];
        }

        // Vanilla's hurt path: armour soaks part of the hit, the mob remembers who hit it and
        // panics or turns on them, and a hit that empties its health starts the death.
        bool damage(std::int32_t id, std::int32_t amount, float away_yaw) {
            auto* found = by_id(id);
            if (found == nullptr || found->dead) {
                return false;
            }
            auto& mob = *found;

            const double dealt = std::max(1.0, armor_reduction(mob.armor, amount));
            mob.health -= dealt;
            ++mob.hurt_serial;
            mob.hurt_ticks = 10;
            mob.knockback = kKnockbackDistance;
            mob.knockback_yaw = away_yaw;
            mob.panic_ticks = kPanicTicks;
            mob.love_ticks = 0;

            if (mob.health <= 0.0) {
                mob.health = 0.0;
                mob.dead = true;
                mob.death_ticks = kDeathTicks;
                LOG_DEBUG(Entity, "{} (id={}) died", mob.type_name, mob.id);
            }
            else {
                LOG_DEBUG(Entity, "{} (id={}) hit for {:.1f}, hp={:.1f}/{:.1f}",
                    mob.type_name, mob.id, dealt, mob.health, mob.max_health);
            }
            return true;
        }

        // Feeding: a player right-clicks the mob with one of its food items. Adults in love
        // look for a partner, and a fed baby grows up a little faster.
        bool feed(std::int32_t id, std::string_view item) {
            auto* found = by_id(id);
            if (found == nullptr || found->dead) {
                return false;
            }
            auto& mob = *found;

            if (!mob.eats(item)) {
                return false;
            }
            if (mob.baby) {
                mob.age_ticks += kFeedingAgeBoost;
                if (mob.age_ticks >= kBabyAge) {
                    grow_up(mob);
                }
                return true;
            }
            if (mob.love_ticks > 0 || mob.breed_cooldown > 0) {
                return false;
            }
            mob.love_ticks = kLoveTicks;
            LOG_DEBUG(Entity, "{} (id={}) is in love", mob.type_name, mob.id);
            return true;
        }

        void tick(const std::vector<PlayerPos>& players,
            Difficulty difficulty = Difficulty::Normal, bool night = true) {
            hits_.clear();

            for (auto& mob : mobs_) {
                if (mob.dead) {
                    if (mob.death_ticks > 0) --mob.death_ticks;
                    continue;
                }

                if (should_discard(mob, players, difficulty)) {
                    mob.dead = true;
                    mob.death_ticks = 0;
                    continue;
                }
                ++mob.no_action_ticks;

                // Daylight decides two things for undead: they keep out of it, and being in
                // it burns them once a second.
                mob.in_sun = !night && mob.undead && shade_resolver_ &&
                    !shade_resolver_(mob.x, mob.y, mob.z);
                if (mob.in_sun && (++mob.sun_ticks % kBurnIntervalTicks) == 0) {
                    mob.health -= kBurnDamage;
                    mob.hurt_ticks = 10;
                    ++mob.hurt_serial;
                    if (mob.health <= 0.0) {
                        mob.health = 0.0;
                        mob.dead = true;
                        mob.death_ticks = kDeathTicks;
                        continue;
                    }
                }

                if (mob.hurt_ticks > 0) --mob.hurt_ticks;
                if (mob.attack_cooldown > 0) --mob.attack_cooldown;
                if (mob.panic_ticks > 0) --mob.panic_ticks;
                if (mob.love_ticks > 0) --mob.love_ticks;
                if (mob.breed_cooldown > 0) --mob.breed_cooldown;
                if (mob.baby) {
                    ++mob.age_ticks;
                    if (mob.age_ticks >= kBabyAge) {
                        grow_up(mob);
                    }
                }

                mob.in_water = water_resolver_ ? water_resolver_(mob.x, mob.z) : false;

                // Knockback wins over steering while it lasts, exactly like a hurt mob.
                if (mob.knockback > 0.0) {
                    const double step = std::min(mob.knockback, kKnockbackStep);
                    mob.knockback -= step;
                    apply_move(mob, mob.knockback_yaw, step, false);
                }
                else {
                    forget_lost_target(mob, players);
                    choose_goal(mob, players);
                    run_goal(mob, players);
                }

                physics(mob);
            }

            const auto before_removal = mobs_.size();
            mobs_.erase(std::remove_if(mobs_.begin(), mobs_.end(),
                [](const Mob& mob) { return mob.dead && mob.death_ticks <= 0; }), mobs_.end());
            if (mobs_.size() != before_removal) {
                rebuild_id_index();
            }
        }

        // The three ways a mob leaves the world without dying: peace, distance, and standing
        // around for too long far from any player.
        bool should_discard(const Mob& mob, const std::vector<PlayerPos>& players,
            Difficulty difficulty) {
            if (mob.persistent || mob.kind == data::MobCategory::Misc) {
                return false;
            }
            if (difficulty == Difficulty::Peaceful && mob.is_monster()) {
                return true;
            }

            double nearest = -1.0;
            for (const auto& player : players) {
                const double dx = mob.x - player.x;
                const double dz = mob.z - player.z;
                const double squared = dx * dx + dz * dz;
                if (nearest < 0.0 || squared < nearest) {
                    nearest = squared;
                }
            }
            if (nearest < 0.0) {
                return false;
            }

            // Water ambient despawns at 64 blocks; every other category at 128.
            const double distance = mob.kind == data::MobCategory::WaterAmbient ? 64.0 : 128.0;
            if (nearest > distance * distance) {
                return true;
            }
            if (nearest < 32.0 * 32.0) {
                return false;
            }
            return mob.no_action_ticks > 600 && (next_random() % 800) == 0;
        }

        const std::vector<Mob>& mobs() const { return mobs_; }
        // The hits that landed during the last tick.
        const std::vector<MobHit>& hits() const { return hits_; }

        std::size_t living_count() const {
            return static_cast<std::size_t>(std::count_if(mobs_.begin(), mobs_.end(),
                [](const Mob& mob) { return !mob.dead; }));
        }

        std::uint32_t& rng() { return rng_state_; }

    private:
        static constexpr double kKnockbackDistance = 0.6;
        static constexpr double kKnockbackStep = 0.15;
        static constexpr int    kDeathTicks = 20;
        static constexpr double kTemptRange = 10.0;
        static constexpr double kTemptSpeed = 1.25;
        static constexpr double kBreedRange = 8.0;
        static constexpr double kBreedDistance = 2.0;
        static constexpr double kFollowParentRange = 8.0;
        static constexpr double kLeapRange = 10.0;
        static constexpr int    kLeapCooldown = 60;
        static constexpr int    kLoveTicks = 600;
        static constexpr int    kBreedCooldown = 600;
        static constexpr int    kFeedingAgeBoost = 240;   // 10% of the 24000 tick childhood

        void grow_up(Mob& mob) {
            mob.baby = false;
            mob.age_ticks = kBabyAge;
            LOG_DEBUG(Entity, "{} (id={}) grew up", mob.type_name, mob.id);
        }

        // ---------------------------------------------------------------- goal selection

        void choose_goal(Mob& mob, const std::vector<PlayerPos>& players) {
            if (mob.spec == nullptr) {
                mob.goal = GoalKind::Stroll;
                return;
            }

            // Every hostile mob in the game acquires players as targets, but the class that
            // registers the target goal is not always the concrete one (a zombie's
            // NearestAttackableTargetGoal is chained with a setter, the skeleton's bow is
            // chosen at runtime from the held item). Those two are supplied here so the
            // behaviour is the same even though the extracted list cannot name them.
            if (mob.is_monster() && mob.target_id == 0) {
                acquire_target(mob, players);
            }
            const bool synthetic_attack = mob.needs_attack_fallback && mob.target_id != 0;
            const GoalKind synthetic_kind = mob.fallback_attack;

            for (int i = 0; i < mob.spec->goal_count; ++i) {
                const auto& spec = data::kGoals[mob.spec->goal_begin + i];
                if (synthetic_attack && spec.priority > kSyntheticAttackPriority) {
                    set_goal(mob, synthetic_kind, 1.0);
                    return;
                }
                const GoalKind kind = goal_kind_from(spec.kind);
                if (kind == GoalKind::Unsupported) {
                    continue;
                }
                if (!can_use(mob, kind, spec, players)) {
                    continue;
                }
                set_goal(mob, kind, static_cast<double>(spec.speed));
                return;
            }
            if (synthetic_attack) {
                set_goal(mob, synthetic_kind, 1.0);
                return;
            }
            // Nothing applies: the mob stands still, like vanilla with an empty selector.
            mob.goal = GoalKind::Unsupported;
        }

        static constexpr int kSyntheticAttackPriority = 2;

        void set_goal(Mob& mob, GoalKind kind, double speed) {
            if (mob.goal != kind) {
                mob.goal = kind;
                mob.goal_age = 0;
            }
            // The per-goal speed modifier is part of the data, and it is why a cow's stroll
            // and a wolf's chase do not run at the same rate.
            mob.goal_speed = speed;
        }

        static bool has_attack_goal(const data::MobSpec& spec) {
            for (int i = 0; i < spec.goal_count; ++i) {
                const auto kind = goal_kind_from(data::kGoals[spec.goal_begin + i].kind);
                if (kind == GoalKind::Melee || kind == GoalKind::Ranged ||
                    kind == GoalKind::Swell || kind == GoalKind::Hop) {
                    return true;
                }
            }
            return false;
        }

        // The families that fight at range in the game hold a bow or a crossbow, and the
        // ranged goals are added when the weapon is equipped rather than in registerGoals.
        static GoalKind default_attack_kind(std::string_view type) {
            if (type.find("skeleton") != std::string_view::npos ||
                type.find("pillager") != std::string_view::npos ||
                type.find("illusioner") != std::string_view::npos) {
                return GoalKind::Ranged;
            }
            return GoalKind::Melee;
        }

        bool can_use(const Mob& mob, GoalKind kind, const data::GoalSpec& spec,
            const std::vector<PlayerPos>& players) const {
            switch (kind) {
            case GoalKind::Float:
                return mob.in_water;
            case GoalKind::Panic:
                return mob.panic_ticks > 0;
            case GoalKind::Melee:
            case GoalKind::Ranged:
                return target_player(mob, players) != nullptr;
            case GoalKind::AttackTarget:
                // The goal targets a class; a player is the only thing we have to offer.
                // Acquiring once is enough, which is what lets the attack goal below it run.
                return spec.target == "Player" && mob.target_id == 0 &&
                    target_player(mob, players) != nullptr;
            case GoalKind::HurtByTarget:
                return mob.attacker_id != 0 && mob.panic_ticks > 0;
            case GoalKind::Avoid:
                // AvoidEntityGoal keeps a distance from a class it fears; a player is the
                // only candidate here, and the creatures that fear players are the passive
                // ones. A creeper's version is about cats, not about running from people.
                return mob.kind != data::MobCategory::Monster &&
                    nearest_player_within(mob, players, 6.0) != nullptr;
            case GoalKind::Tempt:
                return tempting_player(mob, players) != nullptr;
            case GoalKind::Breed:
                return breeding_partner(mob) != nullptr;
            case GoalKind::FollowParent:
                return mob.baby && parent_of(mob) != nullptr;
            case GoalKind::Leap:
                return mob.attack_cooldown <= 0 &&
                    nearest_player_within(mob, players, kLeapRange) != nullptr;
            case GoalKind::Climb:
                // Nothing in this world is climbable yet, so the goal only shows up when the
                // mob is standing in water, where swimming up is what vanilla's climb does.
                return mob.in_water;
            case GoalKind::RestrictSun:
            case GoalKind::FleeSun:
                return mob.in_sun;
            case GoalKind::WaterMove:
                return mob.in_water;
            case GoalKind::Swell:
                // SwellGoal is only usable while the target is close enough to hit.
                return mob.target_id != 0 &&
                    nearest_player_within(mob, players, kSwellRange) != nullptr;
            case GoalKind::Hop:
            case GoalKind::HopDirection:
                // KeepOnJumpingGoal hops all the time, target or no target.
                return true;
            case GoalKind::LookAtPlayer:
                return nearest_player_within(mob, players, kLookDistance) != nullptr;
            case GoalKind::Stand:
                return true;
            case GoalKind::LookAround:
            case GoalKind::Stroll:
            case GoalKind::WaterAvoidingStroll:
                return true;
            default:
                return false;
            }
        }

        void run_goal(Mob& mob, const std::vector<PlayerPos>& players) {
            ++mob.goal_age;
            const double speed = mob.speed_now() * mob.goal_speed;
            switch (mob.goal) {
            case GoalKind::Float:
                mob.vy = std::max(mob.vy, 0.04);   // swim up
                break;
            case GoalKind::Panic:
                wander_away(mob, speed * kPanicSpeed);
                break;
            case GoalKind::Melee:
                chase(mob, players, speed);
                break;
            case GoalKind::Ranged:
                ranged(mob, players, speed);
                break;
            case GoalKind::AttackTarget:
                acquire_target(mob, players);
                break;
            case GoalKind::HurtByTarget:
                // HurtByTargetGoal copies the hurt-by attacker into the attack target.
                mob.target_id = mob.attacker_id;
                chase(mob, players, speed);
                break;
            case GoalKind::Avoid:
                wander_away(mob, speed);
                break;
            case GoalKind::Tempt:
                tempt(mob, players, speed);
                break;
            case GoalKind::Breed:
                breed(mob, speed);
                break;
            case GoalKind::FollowParent:
                follow_parent(mob, speed);
                break;
            case GoalKind::Leap:
                leap(mob, players);
                break;
            case GoalKind::Climb:
                mob.vy = std::max(mob.vy, 0.05);
                stroll(mob, speed, false);
                break;
            case GoalKind::RestrictSun:
            case GoalKind::FleeSun:
                sun_move(mob, speed);
                break;
            case GoalKind::WaterMove:
                water_move(mob, speed);
                break;
            case GoalKind::Hop:
                hop(mob, players, speed);
                break;
            case GoalKind::HopDirection:
                hop_direction(mob);
                break;
            case GoalKind::Swell:
                swell(mob, players);
                break;
            case GoalKind::Stand:
                // The horse family's idle stand is an animation; there is nothing to move.
                break;
            case GoalKind::Stroll:
                stroll(mob, speed, false);
                break;
            case GoalKind::WaterAvoidingStroll:
                stroll(mob, speed, true);
                break;
            case GoalKind::LookAtPlayer:
                look_at_player(mob, players);
                break;
            case GoalKind::LookAround:
                look_around(mob);
                break;
            default:
                break;
            }
        }

        // ---------------------------------------------------------------- goal behaviours

        // A target that died, left the follow range or turned creative is dropped; the next
        // target goal can pick a new one.
        void forget_lost_target(Mob& mob, const std::vector<PlayerPos>& players) {
            if (mob.target_id == 0) {
                return;
            }
            const double limit = static_cast<double>(mob.follow_range) * 1.5;
            for (const auto& player : players) {
                if (player.id != mob.target_id || player.creative) {
                    continue;
                }
                if (distance_squared(mob.x, mob.z, player.x, player.z) <= limit * limit) {
                    return;
                }
            }
            mob.target_id = 0;
        }

        void acquire_target(Mob& mob, const std::vector<PlayerPos>& players) {
            if (mob.target_id != 0) {
                return;
            }
            for (const auto& player : players) {
                if (player.creative) {
                    continue;
                }
                if (distance_squared(mob.x, mob.z, player.x, player.z) <=
                    static_cast<double>(mob.follow_range) * mob.follow_range) {
                    mob.target_id = player.id;
                    return;
                }
            }
        }

        // Undead in daylight look for a roof instead of standing in the open; with no shade
        // to find they simply walk away from the column they are in.
        void sun_move(Mob& mob, double speed) {
            if (mob.wander_ticks <= 0) {
                constexpr double kPi = 3.14159265358979323846;
                bool found = false;
                double target_x = mob.x;
                double target_z = mob.z;
                for (int i = 0; i < 8 && shade_resolver_; ++i) {
                    const double angle = kPi * 0.25 * static_cast<double>(i);
                    const double x = mob.x + std::sin(angle) * 4.0;
                    const double z = mob.z + std::cos(angle) * 4.0;
                    if (shade_resolver_(x, surface_at(x, z) + 1.0, z)) {
                        target_x = x;
                        target_z = z;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    const double angle =
                        kPi * static_cast<double>(next_random() % 360) / 180.0;
                    target_x = mob.x + std::sin(angle) * 6.0;
                    target_z = mob.z + std::cos(angle) * 6.0;
                }
                mob.wander_x = target_x;
                mob.wander_z = target_z;
                mob.wander_ticks = 20;
            }
            --mob.wander_ticks;
            move_towards(mob, mob.wander_x, mob.wander_z, speed);
        }

        // Fish, squid and dolphins swim in three dimensions instead of walking a surface.
        void water_move(Mob& mob, double speed) {
            if (mob.wander_ticks <= 0) {
                constexpr double kPi = 3.14159265358979323846;
                const double angle =
                    kPi * static_cast<double>(next_random() % 360) / 180.0;
                const double radius = 2.0 + static_cast<double>(next_random() % 6);
                mob.wander_x = mob.x + std::sin(angle) * radius;
                mob.wander_z = mob.z + std::cos(angle) * radius;
                mob.hop_x = mob.y + static_cast<double>(next_random() % 5) - 2.0;
                mob.wander_ticks = 40 + static_cast<int>(next_random() % 40);
            }
            --mob.wander_ticks;
            move_towards(mob, mob.wander_x, mob.wander_z, speed * 0.7);
            const double climb = std::clamp((mob.hop_x - mob.y) * 0.05, -0.04, 0.04);
            mob.vy = climb;
        }

        // Slimes keep hopping whether or not anything is around, and hit what they land on.
        void hop(Mob& mob, const std::vector<PlayerPos>& players, double speed) {
            if (mob.hop_ticks > 0) {
                --mob.hop_ticks;
            }
            else if (mob.on_ground) {
                mob.hop_ticks = kHopIntervalTicks / 2 +
                    static_cast<int>(next_random() % kHopIntervalTicks);
                mob.vy = kHopVelocity;
            }

            if (const auto* target = target_player(mob, players)) {
                move_towards(mob, target->x, target->z, speed);
                const double distance =
                    std::sqrt(distance_squared(mob.x, mob.z, target->x, target->z));
                if (distance <= 1.5 && mob.attack_cooldown <= 0) {
                    attack(mob, *target, false);
                }
            }
            else if (mob.hop_x != 0.0 || mob.hop_z != 0.0) {
                move_towards(mob, mob.hop_x, mob.hop_z, speed);
            }
        }

        void hop_direction(Mob& mob) {
            if (mob.wander_ticks > 0) {
                --mob.wander_ticks;
                return;
            }
            constexpr double kPi = 3.14159265358979323846;
            const double angle = kPi * static_cast<double>(next_random() % 360) / 180.0;
            mob.hop_x = mob.x + std::sin(angle) * kStrollRadius;
            mob.hop_z = mob.z + std::cos(angle) * kStrollRadius;
            mob.wander_ticks = kStrollIntervalTicks;
        }

        // Creeper: close in, swell, and take everything nearby with it. The blast damages
        // players; block damage belongs to the explosion work this server does not have.
        void swell(Mob& mob, const std::vector<PlayerPos>& players) {
            const auto* target = target_player(mob, players);
            if (target == nullptr) {
                mob.swell_ticks = std::max(0, mob.swell_ticks - 1);
                return;
            }
            const double distance =
                std::sqrt(distance_squared(mob.x, mob.z, target->x, target->z));
            face(mob, yaw_between(mob.x, mob.z, target->x, target->z));
            if (distance > kSwellRange) {
                mob.swell_ticks = std::max(0, mob.swell_ticks - 1);
                return;
            }

            if (++mob.swell_ticks < kSwellTicks) {
                return;
            }
            for (const auto& player : players) {
                if (player.creative) {
                    continue;
                }
                const double squared = distance_squared(mob.x, mob.z, player.x, player.z);
                if (squared > kExplosionRadius * kExplosionRadius) {
                    continue;
                }
                const double falloff =
                    1.0 - std::sqrt(squared) / (kExplosionRadius * 2.0);
                MobHit hit;
                hit.player = player.id;
                hit.damage = kExplosionDamage * std::max(0.0, falloff);
                hit.ranged = false;
                hit.attacker = mob.type_name;
                hits_.push_back(std::move(hit));
            }
            mob.dead = true;
            mob.death_ticks = 0;
            mob.swell_ticks = 0;
        }

        // ---------------------------------------------------------------- movement

        void chase(Mob& mob, const std::vector<PlayerPos>& players, double speed) {
            if (const auto* target = target_player(mob, players)) {
                const double distance = std::sqrt(distance_squared(mob.x, mob.z, target->x, target->z));
                if (distance > 1.0) {
                    move_towards(mob, target->x, target->z, speed);
                }
                face(mob, yaw_between(mob.x, mob.z, target->x, target->z));
                if (mob.goal == GoalKind::Melee && distance <= kMeleeReach &&
                    mob.attack_cooldown <= 0) {
                    attack(mob, *target, false);
                }
            }
        }

        void ranged(Mob& mob, const std::vector<PlayerPos>& players, double speed) {
            if (const auto* target = target_player(mob, players)) {
                const double distance =
                    std::sqrt(distance_squared(mob.x, mob.z, target->x, target->z));
                // RangedAttackGoal approaches until the target is in range, then holds its
                // ground, and backs off when the target gets too close.
                if (distance > kRangedRange) {
                    move_towards(mob, target->x, target->z, speed);
                }
                else if (distance < kRangedKeepAway) {
                    move_away_from(mob, target->x, target->z, speed);
                }
                face(mob, yaw_between(mob.x, mob.z, target->x, target->z));
                if (distance <= kRangedRange && mob.attack_cooldown <= 0) {
                    attack(mob, *target, true);
                }
            }
        }

        void move_away_from(Mob& mob, double from_x, double from_z, double speed) {
            const float away = yaw_between(from_x, from_z, mob.x, mob.z);
            apply_move(mob, away, speed, true);
        }

        void attack(Mob& mob, const PlayerPos& target, bool ranged) {
            mob.attack_cooldown = ranged ? kRangedCooldownTicks : kMeleeCooldownTicks;
            if (target.creative) {
                // Creative and spectator players take no damage; the mob still swings.
                LOG_TRACE(Entity, "{} (id={}) hits creative player {}", mob.type_name, mob.id,
                    target.id);
                return;
            }

            MobHit hit;
            hit.player = target.id;
            hit.damage = mob.attack_damage;
            hit.ranged = ranged;
            hit.attacker = mob.type_name;
            hits_.push_back(std::move(hit));

            LOG_TRACE(Entity, "{} (id={}) {} player {} for {:.1f}", mob.type_name, mob.id,
                ranged ? "shoots" : "hits", target.id, mob.attack_damage);
        }

        void tempt(Mob& mob, const std::vector<PlayerPos>& players, double speed) {
            if (const auto* player = tempting_player(mob, players)) {
                const double distance = std::sqrt(distance_squared(mob.x, mob.z, player->x, player->z));
                if (distance > 2.0) {
                    move_towards(mob, player->x, player->z, speed * kTemptSpeed);
                }
                face(mob, yaw_between(mob.x, mob.z, player->x, player->z));
            }
        }

        void breed(Mob& mob, double speed) {
            const auto* partner = breeding_partner(mob);
            if (partner == nullptr) {
                return;
            }
            const double distance = std::sqrt(distance_squared(mob.x, mob.z, partner->x, partner->z));
            if (distance > kBreedDistance) {
                move_towards(mob, partner->x, partner->z, speed);
                return;
            }
            // Two adults in love that meet make one child; both lose their love and wait out
            // the breed cooldown, which is what BreedGoal#spawnChildAt does.
            spawn_child(mob, *partner);
        }

        void follow_parent(Mob& mob, double speed) {
            const auto* parent = parent_of(mob);
            if (parent == nullptr) {
                return;
            }
            const double distance = std::sqrt(distance_squared(mob.x, mob.z, parent->x, parent->z));
            if (distance > 3.0) {
                move_towards(mob, parent->x, parent->z, speed * 1.1);
            }
        }

        void leap(Mob& mob, const std::vector<PlayerPos>& players) {
            if (const auto* target = nearest_player_within(mob, players, kLeapRange)) {
                if (mob.on_ground) {
                    mob.vy = kJumpVelocity;
                    move_towards(mob, target->x, target->z, mob.speed_now() * 1.5);
                }
                mob.attack_cooldown = kLeapCooldown;
            }
        }

        void stroll(Mob& mob, double speed, bool avoid_water) {
            if (mob.wander_ticks <= 0) {
                const int angle = static_cast<int>(next_random() % 360);
                const double radius = 1.0 + static_cast<double>(next_random() % 10);
                constexpr double kPi = 3.14159265358979323846;
                const double radians = angle * kPi / 180.0;
                mob.wander_x = mob.x - std::sin(radians) * radius;
                mob.wander_z = mob.z + std::cos(radians) * radius;
                mob.wander_ticks = kStrollIntervalTicks + static_cast<int>(next_random() % 80);
            }
            --mob.wander_ticks;

            if (avoid_water && water_resolver_ && water_resolver_(mob.wander_x, mob.wander_z)) {
                mob.wander_ticks = 0;
                return;
            }
            if (distance_squared(mob.x, mob.z, mob.wander_x, mob.wander_z) > 0.25) {
                move_towards(mob, mob.wander_x, mob.wander_z, speed * 0.8);
            }
            else {
                mob.wander_ticks = 0;
            }
        }

        void wander_away(Mob& mob, double speed) {
            // knockback_yaw points from whatever hurt the mob towards the mob, so running
            // along it is running away from the attacker (PanicGoal does the same).
            const float away = mob.knockback_yaw;
            if (mob.panic_ticks % 20 == 0 || mob.wander_ticks <= 0) {
                mob.wander_x = mob.x - std::sin(away * 3.14159265f / 180.0f) * 6.0;
                mob.wander_z = mob.z + std::cos(away * 3.14159265f / 180.0f) * 6.0;
                mob.wander_ticks = 20;
            }
            --mob.wander_ticks;
            move_towards(mob, mob.wander_x, mob.wander_z, speed);
        }

        // LookAtPlayerGoal turns the head, not the body: the mob keeps walking where it was
        // going and only its head follows the player.
        void look_at_player(Mob& mob, const std::vector<PlayerPos>& players) {
            if (const auto* player = nearest_player_within(mob, players, kLookDistance)) {
                mob.head_yaw = yaw_between(mob.x, mob.z, player->x, player->z);
                mob.pitch = static_cast<float>(std::clamp(
                    -(player->y - (mob.y + 1.6)) * 10.0, -40.0, 40.0));
            }
        }

        void look_around(Mob& mob) {
            if (mob.look_ticks > 0) {
                --mob.look_ticks;
                return;
            }
            mob.look_ticks = kLookIntervalTicks + static_cast<int>(next_random() % kLookIntervalTicks);
            mob.look_yaw = static_cast<float>(next_random() % 360);
            mob.look_pitch = static_cast<float>(static_cast<int>(next_random() % 40) - 20);
            mob.head_yaw = mob.look_yaw;
            mob.pitch = mob.look_pitch;
        }

        void face(Mob& mob, float yaw) {
            mob.yaw = yaw;
            mob.head_yaw = yaw;
        }

        bool move_towards(Mob& mob, double target_x, double target_z, double speed) {
            const float yaw = yaw_between(mob.x, mob.z, target_x, target_z);
            face(mob, yaw);
            return apply_move(mob, yaw, speed, true);
        }

        bool apply_move(Mob& mob, float yaw, double speed, bool can_jump) {
            // Entity.getInputVector: the local forward (0, 0, 1) becomes (-sin, cos), so the
            // mob walks exactly where its yaw points instead of away from it.
            constexpr double kPi = 3.14159265358979323846;
            const double radians = static_cast<double>(yaw) * kPi / 180.0;
            const double nx = mob.x - std::sin(radians) * speed;
            const double nz = mob.z + std::cos(radians) * speed;
            if (!inside_roam(nx, nz)) {
                return false;
            }

            const double ground = surface_at(nx, nz);
            if (ground - mob.y > kStepHeight) {
                // A wall: vanilla would take the jump control if the path asks for it.
                if (can_jump && mob.on_ground) {
                    mob.vy = kJumpVelocity;
                    mob.jump_ticks = 10;
                }
                mob.wander_ticks = 0;
                return false;
            }

            mob.x = nx;
            mob.z = nz;
            mob.vx = -std::sin(radians) * speed;
            mob.vz = std::cos(radians) * speed;
            return true;
        }

        // Gravity, drag and the ground check, in LivingEntity's numbers.
        void physics(Mob& mob) {
            if (mob.jump_ticks > 0) --mob.jump_ticks;

            mob.vy -= kGravity;
            if (mob.in_water) {
                mob.vy = std::max(mob.vy, -0.05);
                mob.vx *= 0.8;
                mob.vz *= 0.8;
            }
            mob.y += mob.vy;

            const double ground = surface_at(mob.x, mob.z);
            if (mob.y <= ground) {
                mob.y = ground;
                mob.vy = 0.0;
                mob.on_ground = true;
                // Ground friction bleeds the horizontal velocity out when nothing drives it.
                mob.vx *= kGroundFriction;
                mob.vz *= kGroundFriction;
            }
            else {
                mob.on_ground = false;
                mob.vx *= kDrag;
                mob.vz *= kDrag;
            }
        }

        bool inside_roam(double x, double z) const {
            if (roam_radius_ <= 0.0) {
                return true;
            }
            const double dx = x - roam_x_;
            const double dz = z - roam_z_;
            return dx * dx + dz * dz <= roam_radius_ * roam_radius_;
        }

        double surface_at(double x, double z) const {
            return ground_resolver_ ? ground_resolver_(x, z) : ground_y_;
        }

        void ground_at(Mob& mob) {
            mob.y = surface_at(mob.x, mob.z);
            mob.on_ground = true;
            mob.vy = 0.0;
        }

        // ---------------------------------------------------------------- targeting

        const PlayerPos* nearest_player_within(const Mob& mob,
            const std::vector<PlayerPos>& players, double range) const {
            const PlayerPos* best = nullptr;
            double best_sq = range * range;
            for (const auto& player : players) {
                const double dist_sq = distance_squared(mob.x, mob.z, player.x, player.z);
                if (dist_sq <= best_sq) {
                    best_sq = dist_sq;
                    best = &player;
                }
            }
            return best;
        }

        const PlayerPos* target_player(const Mob& mob,
            const std::vector<PlayerPos>& players) const {
            if (mob.follow_range <= 0.0f) {
                return nullptr;
            }
            const PlayerPos* best = nullptr;
            double best_sq = static_cast<double>(mob.follow_range) * mob.follow_range;
            for (const auto& player : players) {
                // Creative and spectator players are not a target.
                if (player.creative) {
                    continue;
                }
                if (mob.target_id != 0 && player.id != mob.target_id) {
                    continue;
                }
                const double dist_sq = distance_squared(mob.x, mob.z, player.x, player.z);
                if (dist_sq <= best_sq) {
                    best_sq = dist_sq;
                    best = &player;
                }
            }
            return best;
        }

        // TemptGoal only follows a player holding something the mob eats.
        const PlayerPos* tempting_player(const Mob& mob,
            const std::vector<PlayerPos>& players) const {
            const PlayerPos* best = nullptr;
            double best_sq = kTemptRange * kTemptRange;
            for (const auto& player : players) {
                if (player.held_item.empty() || !mob.eats(player.held_item)) {
                    continue;
                }
                const double dist_sq = distance_squared(mob.x, mob.z, player.x, player.z);
                if (dist_sq <= best_sq) {
                    best_sq = dist_sq;
                    best = &player;
                }
            }
            return best;
        }

        const Mob* breeding_partner(const Mob& mob) const {
            if (mob.baby || mob.love_ticks <= 0) {
                return nullptr;
            }
            const Mob* best = nullptr;
            double best_sq = kBreedRange * kBreedRange;
            for (const auto& other : mobs_) {
                if (&other == &mob || other.dead || other.baby || other.love_ticks <= 0 ||
                    other.type_name != mob.type_name) {
                    continue;
                }
                const double dist_sq = distance_squared(mob.x, mob.z, other.x, other.z);
                if (dist_sq <= best_sq) {
                    best_sq = dist_sq;
                    best = &other;
                }
            }
            return best;
        }

        const Mob* parent_of(const Mob& mob) const {
            const Mob* best = nullptr;
            double best_sq = kFollowParentRange * kFollowParentRange;
            for (const auto& other : mobs_) {
                if (&other == &mob || other.dead || other.baby || other.type_name != mob.type_name) {
                    continue;
                }
                const double dist_sq = distance_squared(mob.x, mob.z, other.x, other.z);
                if (dist_sq <= best_sq) {
                    best_sq = dist_sq;
                    best = &other;
                }
            }
            return best;
        }

        void spawn_child(Mob& parent, const Mob& partner) {
            parent.love_ticks = 0;
            parent.breed_cooldown = kBreedCooldown;
            for (auto& other : mobs_) {
                if (&other == &partner) {
                    other.love_ticks = 0;
                    other.breed_cooldown = kBreedCooldown;
                    break;
                }
            }
            const double x = (parent.x + partner.x) * 0.5;
            const double z = (parent.z + partner.z) * 0.5;
            if (spawn(parent.type_name, x, z, true)) {
                LOG_DEBUG(Entity, "{} bred at ({:.1f}, {:.1f})", parent.type_name, x, z);
            }
        }

        Mob* by_id(std::int32_t id) {
            const auto found = id_index_.find(id);
            return found == id_index_.end() ? nullptr : &mobs_[found->second];
        }

        // Erasing shifts every index behind the removed mob, so the table is rebuilt once per
        // tick that actually removed something instead of being patched per entry.
        void rebuild_id_index() {
            id_index_.clear();
            id_index_.reserve(mobs_.size());
            for (std::size_t i = 0; i < mobs_.size(); ++i) {
                id_index_[mobs_[i].id] = i;
            }
        }

        std::uint32_t next_random() {
            rng_state_ = rng_state_ * 1664525u + 1013904223u;
            return rng_state_ >> 8;
        }

        std::vector<Mob> mobs_;
        std::unordered_map<std::int32_t, std::size_t> id_index_;
        std::vector<MobHit> hits_;
        GroundResolver ground_resolver_;
        WaterResolver water_resolver_;
        ShadeResolver shade_resolver_;
        double ground_y_ = 0.0;
        double roam_x_ = 0.0;
        double roam_z_ = 0.0;
        double roam_radius_ = 0.0;
        const std::unordered_map<std::string, std::int32_t>* type_ids_ = nullptr;
        std::uint32_t rng_state_ = 0x9E3779B9u;
        std::int32_t next_id_ = 1;
    };

} // namespace terila::entity
