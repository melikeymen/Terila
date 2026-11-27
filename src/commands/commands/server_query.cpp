#include "commands/registry.hpp"
#include "version/mc_version.hpp"

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace terila::commands {

    namespace {

        constexpr std::size_t kUsageLines = 12;

        Result run_seed(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }
            const std::int64_t seed = origin.game->seed();
            origin.say(std::format("Seed: [{}]", seed));
            return Result{ static_cast<std::int32_t>(seed), true };
        }

        Result run_list(Exec& exec) {
            Origin& origin = exec.origin;
            if (origin.game == nullptr) {
                return Result{ 0, false };
            }

            const auto players = origin.game->players();
            std::string names;
            for (const auto& player : players) {
                if (!names.empty()) {
                    names += ", ";
                }
                names += player.name;
            }
            origin.say(std::format("There are {} of a max of {} players online: {}",
                players.size(), origin.game->max_players(), names));
            return Result{ static_cast<std::int32_t>(players.size()), true };
        }

        Result run_version(Exec& exec) {
            exec.origin.say(std::format("This server is Terila, speaking Minecraft {}",
                terila::version::label()));
            return Result{ 1, true };
        }

        Result run_help(Exec& exec) {
            Origin& origin = exec.origin;
            const auto* name = exec.args.get<std::string>("command");

            if (name == nullptr) {
                const auto names = exec.registry.command_names();
                std::string joined;
                for (const auto& entry : names) {
                    if (!joined.empty()) {
                        joined += ", ";
                    }
                    joined += entry;
                }
                origin.say("Commands: " + joined);
                origin.say("Run /help <command> to see how one is written");
                return Result{ static_cast<std::int32_t>(names.size()), true };
            }

            const auto lines = exec.registry.usage_lines(*name, kUsageLines);
            if (lines.empty()) {
                origin.say("No such command '" + *name + "'");
                return Result{ 0, false };
            }
            for (const auto& line : lines) {
                origin.say(line);
            }
            return Result{ static_cast<std::int32_t>(lines.size()), true };
        }

    } // namespace

    void install_query(Registry& registry) {
        registry.root().literal("seed", Capability::Operator).executes(run_seed);
        registry.root().literal("list").executes(run_list);
        registry.root().literal("version").executes(run_version);

        Node& help = registry.root().literal("help");
        help.executes(run_help);
        help.argument("command", Parser::text()).executes(run_help);

        // The game answers "?" the same way it answers "help".
        Node& question = registry.root().literal("?");
        question.executes(run_help);
        question.argument("command", Parser::text()).executes(run_help);
    }

} // namespace terila::commands
