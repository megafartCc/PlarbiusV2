#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "plarbius/eval/selfplay_evaluator.hpp"
#include "plarbius/game/game.hpp"
#include "plarbius/games/kuhn/kuhn_game.hpp"
#include "plarbius/games/leduc/leduc_game.hpp"
#include "plarbius/policy/policy_io.hpp"

namespace {

struct CliOptions {
  std::string game = "kuhn";
  std::string policy_a_path;
  std::string policy_b_path;
  std::size_t threads = 1;
};

[[nodiscard]] bool LooksLikeFlag(std::string_view value) {
  return value.rfind("--", 0) == 0;
}

[[nodiscard]] std::uint64_t ParseUnsigned(const std::string& text, const char* name) {
  try {
    return std::stoull(text);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string("Invalid value for ") + name + ": " + text);
  }
}

void PrintUsage() {
  std::cout << "Usage:\n"
            << "  plarbius_selfplay --policy-a path [--policy-b path] [--game kuhn|leduc]\n"
            << "                   [--threads N]\n";
}

std::unique_ptr<plarbius::game::Game> MakeGame(const std::string& game_name) {
  if (game_name == "kuhn") {
    return std::make_unique<plarbius::games::kuhn::KuhnGame>();
  }
  if (game_name == "leduc") {
    return std::make_unique<plarbius::games::leduc::LeducGame>();
  }
  throw std::invalid_argument("Unsupported game: " + game_name);
}

CliOptions ParseCli(int argc, char** argv) {
  CliOptions options;

  int index = 1;
  while (index < argc) {
    const std::string flag = argv[index++];
    if (!LooksLikeFlag(flag)) {
      throw std::invalid_argument("Unknown positional argument: " + flag);
    }

    if (flag == "--policy-a") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --policy-a.");
      }
      options.policy_a_path = argv[index++];
    } else if (flag == "--policy-b") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --policy-b.");
      }
      options.policy_b_path = argv[index++];
    } else if (flag == "--game") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --game.");
      }
      options.game = argv[index++];
    } else if (flag == "--threads") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --threads.");
      }
      options.threads = static_cast<std::size_t>(ParseUnsigned(argv[index++], "--threads"));
    } else if (flag == "--help" || flag == "-h") {
      PrintUsage();
      std::exit(0);
    } else {
      throw std::invalid_argument("Unknown flag: " + flag);
    }
  }

  if (options.policy_a_path.empty()) {
    throw std::invalid_argument("--policy-a is required.");
  }
  if (options.policy_b_path.empty()) {
    options.policy_b_path = options.policy_a_path;
  }
  if (options.threads == 0) {
    options.threads = 1;
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;
  try {
    options = ParseCli(argc, argv);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    PrintUsage();
    return 1;
  }

  try {
    auto game = MakeGame(options.game);
    const auto policy_a = plarbius::policy::LoadPolicy(options.policy_a_path);
    const auto policy_b = plarbius::policy::LoadPolicy(options.policy_b_path);
    const auto report =
        plarbius::eval::EvaluateExpectedSelfplay(*game, policy_a, policy_b, options.threads);

    std::cout << "game=" << options.game << '\n';
    std::cout << "policy_a=" << options.policy_a_path << '\n';
    std::cout << "policy_b=" << options.policy_b_path << '\n';
    std::cout << "threads=" << options.threads << '\n';
    std::cout << "utility_p0=" << std::fixed << std::setprecision(6) << report.utility_p0 << '\n';
    std::cout << "utility_p1=" << std::fixed << std::setprecision(6) << report.utility_p1 << '\n';
    std::cout << "evaluated_states=" << report.evaluated_states << '\n';
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}

