#include "commands/registry.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace terila::commands {

    namespace {

        Result run_damage(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            const auto* selector = exec.args.get<Selector>("target");
            const auto* amount = exec.args.get<double>("amount");
            if (selector == nullptr || amount == nullptr) {
                return Result{ 0, false };
            }
            if (*amount <= 0.0) {
                origin.say("The damage must be greater than zero");
                return Result{ 0, false };
            }

            std::string error;
            const auto targets = targets_of(*selector, origin, error);
            if (targets.empty()) {
                origin.say(error.empty() ? "No entity was found" : error);
                return Result{ 0, false };
            }

            const std::string cause = [&] {
                if (const auto* type = exec.args.get<std::string>("damageType")) {
                    return *type;
                }
                return std::string("generic");
            }();

            std::int32_t hurt = 0;
            for (const auto& target : targets) {
                if (!origin.game->damage(target.entity, static_cast<float>(*amount), cause)) {
                    continue;
                }
                ++hurt;
                origin.say(std::format("Applied {} damage to {}", *amount, target.name));
            }

            if (hurt == 0) {
                origin.say("Nothing took damage");
                return Result{ 0, false };
            }
            return Result{ hurt, true };
        }

    } // namespace

    void install_damage(Registry& registry) {
        Node& damage = registry.root().literal("damage", Capability::Operator);
        Node& target = damage.argument("target", Parser::entities());
        Node& amount = target.argument("amount", Parser::real(0.0));
        amount.executes(run_damage);
        amount.argument("damageType", Parser::resource_location()).executes(run_damage);
    }

} // namespace terila::commands
