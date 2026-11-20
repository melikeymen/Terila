#pragma once

// The vocabulary the mob AI is written in: vanilla's goal names, the movement constants
// LivingEntity/Mob use, and the small helpers those goals share. The per-mob goal list
// itself is data (src/entity/mob_data.hpp), extracted from the decompiled classes, so a mob
// behaves the way its Java class registers it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace terila::entity {

    // Goal classes this server implements. Everything else the decompiled source registers
    // (villager trading, raids, taming, sitting, boss spells, ...) is ignored: those need
    // systems this server does not have yet, and a mob without them simply keeps its lower
    // goals.
    enum class GoalKind : std::uint8_t {
        Float,                  // swim up in water
        Panic,                  // run from whatever hurt us
        Melee,                  // walk to the target and hit it
        Ranged,                 // keep distance and shoot
        Stroll,                 // wander to a random spot
        WaterAvoidingStroll,    // wander, but not into water
        LookAtPlayer,           // turn the head towards a nearby player
        LookAround,             // idle head movement
        FollowParent,           // baby follows an adult of its kind
        Tempt,                  // follow the player holding its food
        Breed,                  // walk to the partner in love
        Avoid,                  // keep away from a creature it fears
        AttackTarget,           // pick a player as the attack target
        HurtByTarget,           // remember who hit us
        Leap,                   // jump at the target
        Climb,                  // climb when in a climbable block
        RestrictSun,            // undead that keeps out of the sun
        FleeSun,                // undead that runs from the sun
        WaterMove,              // fish, squid and dolphins: swim in three dimensions
        Hop,                    // slime family: keep hopping towards the target
        HopDirection,           // slime family: pick the random hop direction
        Swell,                  // creeper: swell up, then explode
        Stand,                  // horse family: the idle stand animation
        Unsupported,
    };

    inline GoalKind goal_kind_from(std::string_view name) noexcept {
        if (name == "FloatGoal" || name == "CubeMobFloatGoal" || name == "FoxFloatGoal" ||
            name == "BreathAirGoal" || name == "ClimbOnTopOfPowderSnowGoal") {
            return GoalKind::Float;
        }
        if (name == "PanicGoal" || name == "MountPanicGoal" || name == "PanicGoalBaby" ||
            name == "TamableAnimalPanicGoal" || name == "FoxPanicGoal" ||
            name == "RunAroundLikeCrazyGoal" || name == "RabbitPanicGoal" ||
            name == "TurtlePanicGoal" || name == "PandaPanicGoal") {
            return GoalKind::Panic;
        }
        if (name == "MeleeAttackGoal" || name == "ZombieAttackGoal" ||
            name == "SpiderAttackGoal" || name == "OcelotAttackGoal" ||
            name == "GuardianAttackGoal" || name == "HoldGroundAttackGoal" ||
            name == "LlamaAttackWolfGoal" || name == "SpearUseGoal" ||
            name == "FoxMeleeAttackGoal" || name == "PandaAttackGoal" ||
            name == "PolarBearMeleeAttackGoal" || name == "PolarBearAttackPlayersGoal") {
            return GoalKind::Melee;
        }
        if (name == "RangedAttackGoal" || name == "RangedBowAttackGoal" ||
            name == "RangedCrossbowAttackGoal" || name == "BlazeAttackGoal") {
            return GoalKind::Ranged;
        }
        if (name == "RandomStrollGoal" || name == "MoveTowardsRestrictionGoal" ||
            name == "MoveThroughVillageGoal" || name == "RaiderMoveThroughVillageGoal" ||
            name == "StalkPreyGoal" || name == "MoveToItemGoal" ||
            name == "PlayWithItemsGoal" || name == "BeeWanderGoal" ||
            name == "FoxStrollThroughVillageGoal" || name == "TurtleRandomStrollGoal" ||
            name == "ParrotWanderGoal" || name == "FollowMobGoal") {
            return GoalKind::Stroll;
        }
        if (name == "WaterAvoidingRandomStrollGoal") return GoalKind::WaterAvoidingStroll;
        if (name == "LookAtPlayerGoal" || name == "FoxLookAtPlayerGoal") {
            return GoalKind::LookAtPlayer;
        }
        if (name == "RandomLookAroundGoal") return GoalKind::LookAround;
        if (name == "FollowParentGoal" || name == "FoxFollowParentGoal") {
            return GoalKind::FollowParent;
        }
        if (name == "TemptGoal" || name == "BegGoal") return GoalKind::Tempt;
        if (name == "BreedGoal" || name == "FoxBreedGoal" || name == "PandaBreedGoal" ||
            name == "TurtleBreedGoal") {
            return GoalKind::Breed;
        }
        if (name == "AvoidEntityGoal") return GoalKind::Avoid;
        if (name == "NearestAttackableTargetGoal" || name == "NonTameRandomTargetGoal" ||
            name == "ResetUniversalAngerTargetGoal" || name == "NearestAttackableWitchTargetGoal" ||
            name == "DefendVillageTargetGoal") {
            return GoalKind::AttackTarget;
        }
        if (            name == "HurtByTargetGoal" || name == "LlamaHurtByTargetGoal" ||
            name == "BeeHurtByOtherGoal" || name == "PandaHurtByTargetGoal" ||
            name == "PolarBearHurtByTargetGoal") {
            return GoalKind::HurtByTarget;
        }
        if (name == "LeapAtTargetGoal" || name == "FoxPounceGoal") return GoalKind::Leap;
        if (name == "RestrictSunGoal") return GoalKind::RestrictSun;
        if (name == "FleeSunGoal") return GoalKind::FleeSun;
        if (name == "FishSwimGoal" || name == "RandomSwimmingGoal" ||
            name == "SquidRandomMovementGoal" || name == "SquidFleeGoal" ||
            name == "DolphinSwimToTreasureGoal" || name == "DolphinSwimWithPlayerGoal" ||
            name == "DolphinJumpGoal" || name == "RandomFloatAroundGoal" ||
            name == "TryFindLiquidGoal" || name == "FollowFlockLeaderGoal") {
            return GoalKind::WaterMove;
        }
        if (name == "CubeMobKeepOnJumpingGoal" || name == "SlimeKeepOnJumpingGoal") {
            return GoalKind::Hop;
        }
        // The direction goal only chooses where the next hop goes; the hop goal below it
        // does the jumping, so both map to the same behaviour here.
        if (name == "CubeMobRandomDirectionGoal") return GoalKind::Hop;
        if (name == "SwellGoal") return GoalKind::Swell;
        if (name == "RandomStandGoal") return GoalKind::Stand;
        return GoalKind::Unsupported;
    }

    // LivingEntity / Mob constants, in blocks per tick.
    inline constexpr double kGravity = 0.08;
    inline constexpr double kDrag = 0.98;
    inline constexpr double kGroundFriction = 0.6 * 0.91;   // slipperiness * 0.91
    inline constexpr double kJumpVelocity = 0.42;
    inline constexpr double kStepHeight = 0.6;

    // How a goal measures things, straight out of the Java defaults.
    inline constexpr double kMeleeReach = 2.0;
    inline constexpr int kMeleeCooldownTicks = 20;     // attack speed 4
    inline constexpr int kRangedCooldownTicks = 20;    // skeleton RangedAttackGoal interval
    inline constexpr double kRangedRange = 15.0;
    inline constexpr double kRangedKeepAway = 6.0;     // backs off inside this
    inline constexpr int kPanicTicks = 100;
    inline constexpr double kPanicSpeed = 1.25;        // PanicGoal(this, 1.25)
    inline constexpr int kStrollIntervalTicks = 120;   // RandomStrollGoal every 120 ticks
    inline constexpr double kStrollRadius = 10.0;
    inline constexpr int kLookIntervalTicks = 40;
    inline constexpr double kLookDistance = 8.0;
    inline constexpr int kBabyAge = 24000;             // vanilla baby -> adult

    // Creeper: 30 ticks of swelling from three blocks, then a 3 block blast.
    inline constexpr int kSwellTicks = 30;
    inline constexpr double kSwellRange = 3.0;
    inline constexpr double kExplosionRadius = 3.0;
    inline constexpr double kExplosionDamage = 12.0;

    // Slime family hop cadence.
    inline constexpr int kHopIntervalTicks = 30;
    inline constexpr double kHopVelocity = 0.42;

    // Undead burn in daylight: one point every twenty ticks, like the fire tick.
    inline constexpr int kBurnIntervalTicks = 20;
    inline constexpr double kBurnDamage = 1.0;

    // Undead burn in sunlight and keep out of it; the list is the game's own family.
    inline bool is_undead(std::string_view type) noexcept {
        return type == "minecraft:zombie" || type == "minecraft:zombie_villager" ||
            type == "minecraft:husk" || type == "minecraft:drowned" ||
            type == "minecraft:skeleton" || type == "minecraft:stray" ||
            type == "minecraft:wither_skeleton" || type == "minecraft:zombie_horse" ||
            type == "minecraft:skeleton_horse" || type == "minecraft:phantom";
    }

    // Vanilla armour: 0..20 points, each 4% up to 80%, then dimishing with the hit size.
    inline double armor_reduction(float armor, double damage) noexcept {
        const double points = std::clamp(static_cast<double>(armor) / 5.0,
            static_cast<double>(armor) - damage / (2.0 + armor / 16.0), 20.0);
        return damage * (1.0 - std::max(points, 0.0) / 25.0);
    }

    inline double distance_squared(double ax, double az, double bx, double bz) noexcept {
        const double dx = bx - ax;
        const double dz = bz - az;
        return dx * dx + dz * dz;
    }

    // Degrees around +Y in the game's convention: 0 faces +Z (south), 90 faces -X (west),
    // 180 faces -Z (north) and 270 faces +X (east). MoveControl derives it as
    // atan2(dz, dx) - 90, which is the atan2(-dx, dz) below, and Entity.getInputVector turns
    // it back into (-sin, cos): that pair is what makes a mob both look and walk the same way.
    inline float yaw_between(double from_x, double from_z, double to_x, double to_z) {
        constexpr double kPi = 3.14159265358979323846;
        const double dx = to_x - from_x;
        const double dz = to_z - from_z;
        double degrees = std::atan2(-dx, dz) * 180.0 / kPi;
        while (degrees < 0.0) degrees += 360.0;
        while (degrees >= 360.0) degrees -= 360.0;
        return static_cast<float>(degrees);
    }

} // namespace terila::entity
