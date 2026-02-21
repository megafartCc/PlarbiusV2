#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "plarbius/core/types.hpp"
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
  std::uint64_t sampled_hands = 0;
  std::uint64_t sample_seed = 1;
  std::string hand_history_out_path;
};

struct SampledHandRow {
  std::uint64_t hand_id = 0;
  double delta_p0_bb = 0.0;
  std::string state_key;
  std::string action_bucket;
  std::string position;
  std::string street;
  std::string p0_card;
  std::string p1_card;
};

struct SampledSelfplayReport {
  std::uint64_t hands = 0;
  double utility_p0_mean = 0.0;
  double utility_p1_mean = 0.0;
  double ci95 = 0.0;
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
            << "                   [--threads N] [--sampled-hands N] [--sample-seed N]\n"
            << "                   [--hand-history-out path]\n";
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
    } else if (flag == "--sampled-hands") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --sampled-hands.");
      }
      options.sampled_hands = ParseUnsigned(argv[index++], "--sampled-hands");
    } else if (flag == "--sample-seed") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --sample-seed.");
      }
      options.sample_seed = ParseUnsigned(argv[index++], "--sample-seed");
    } else if (flag == "--hand-history-out") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --hand-history-out.");
      }
      options.hand_history_out_path = argv[index++];
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
  if (!options.hand_history_out_path.empty() && options.sampled_hands == 0) {
    throw std::invalid_argument("--hand-history-out requires --sampled-hands > 0.");
  }
  return options;
}

std::string EscapeCsv(const std::string& value) {
  if (value.find_first_of(",\"\n\r") == std::string::npos) {
    return value;
  }
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (char ch : value) {
    if (ch == '"') {
      escaped.push_back('"');
      escaped.push_back('"');
    } else {
      escaped.push_back(ch);
    }
  }
  escaped.push_back('"');
  return escaped;
}

std::size_t SampleIndex(const std::vector<double>& probabilities, std::mt19937_64* rng) {
  if (probabilities.empty()) {
    throw std::runtime_error("Cannot sample from empty probability vector.");
  }

  std::uniform_real_distribution<double> unit(0.0, 1.0);
  const double draw = unit(*rng);

  double running = 0.0;
  for (std::size_t i = 0; i < probabilities.size(); ++i) {
    running += std::max(0.0, probabilities[i]);
    if (draw <= running) {
      return i;
    }
  }
  return probabilities.size() - 1;
}

char ActionHistoryToken(plarbius::game::ActionType type) {
  switch (type) {
    case plarbius::game::ActionType::kCheck:
      return 'c';
    case plarbius::game::ActionType::kBet:
      return 'b';
    case plarbius::game::ActionType::kCall:
      return 'c';
    case plarbius::game::ActionType::kFold:
      return 'f';
  }
  return '?';
}

std::string ExtractCardFromInfoset(const std::string& infoset_key) {
  const std::size_t first_pipe = infoset_key.find('|');
  if (first_pipe == std::string::npos) {
    return {};
  }
  const std::size_t second_pipe = infoset_key.find('|', first_pipe + 1);
  if (second_pipe == std::string::npos || second_pipe <= first_pipe + 1) {
    return {};
  }
  return infoset_key.substr(first_pipe + 1, second_pipe - first_pipe - 1);
}

double SampleSingleHand(const plarbius::game::Game& game,
                        const plarbius::policy::PolicyTable& policy_a,
                        const plarbius::policy::PolicyTable& policy_b,
                        std::mt19937_64* rng,
                        std::string* terminal_history,
                        std::string* first_infoset_p0,
                        std::string* first_infoset_p1) {
  auto state = game.NewInitialState();
  terminal_history->clear();
  first_infoset_p0->clear();
  first_infoset_p1->clear();

  while (!state->IsTerminal()) {
    if (state->IsChanceNode()) {
      const auto outcomes = state->ChanceOutcomes();
      if (outcomes.empty()) {
        throw std::runtime_error("Chance node has no outcomes.");
      }

      std::vector<double> probabilities;
      probabilities.reserve(outcomes.size());
      for (const auto& outcome : outcomes) {
        probabilities.push_back(outcome.probability);
      }

      const std::size_t sampled = SampleIndex(probabilities, rng);
      state = state->CloneAndApplyChance(outcomes[sampled].id);
      continue;
    }

    const auto actions = state->LegalActions();
    if (actions.empty()) {
      throw std::runtime_error("Decision node has no legal actions.");
    }

    const auto current_player = state->CurrentPlayer();
    const auto infoset_key = state->InfosetKey(current_player);
    if (current_player == plarbius::kPlayer0 && first_infoset_p0->empty()) {
      *first_infoset_p0 = infoset_key;
    }
    if (current_player == plarbius::kPlayer1 && first_infoset_p1->empty()) {
      *first_infoset_p1 = infoset_key;
    }

    const auto action_probs = plarbius::policy::GetActionDistribution(
        current_player == plarbius::kPlayer0 ? policy_a : policy_b,
        infoset_key,
        actions.size());

    const std::size_t sampled_action = SampleIndex(action_probs, rng);
    const auto action = actions[sampled_action];
    terminal_history->push_back(ActionHistoryToken(action.type));
    state = state->CloneAndApplyAction(action);
  }

  return state->TerminalUtility(plarbius::kPlayer0);
}

SampledSelfplayReport EvaluateSampledSelfplay(const plarbius::game::Game& game,
                                              const plarbius::policy::PolicyTable& policy_a,
                                              const plarbius::policy::PolicyTable& policy_b,
                                              std::uint64_t hands,
                                              std::uint64_t sample_seed,
                                              std::vector<SampledHandRow>* rows) {
  if (hands == 0) {
    return {};
  }

  rows->clear();
  rows->reserve(static_cast<std::size_t>(hands));

  std::mt19937_64 rng(sample_seed);
  double sum = 0.0;
  double sum_sq = 0.0;

  for (std::uint64_t hand_id = 1; hand_id <= hands; ++hand_id) {
    std::string terminal_history;
    std::string first_infoset_p0;
    std::string first_infoset_p1;

    const double utility_p0 = SampleSingleHand(
        game,
        policy_a,
        policy_b,
        &rng,
        &terminal_history,
        &first_infoset_p0,
        &first_infoset_p1);

    sum += utility_p0;
    sum_sq += utility_p0 * utility_p0;

    SampledHandRow row;
    row.hand_id = hand_id;
    row.delta_p0_bb = utility_p0;
    row.state_key = terminal_history.empty() ? "_" : terminal_history;
    row.action_bucket = row.state_key;
    row.position = "p0_first";
    row.street = "kuhn";
    row.p0_card = ExtractCardFromInfoset(first_infoset_p0);
    row.p1_card = ExtractCardFromInfoset(first_infoset_p1);
    rows->push_back(std::move(row));
  }

  const double mean = sum / static_cast<double>(hands);
  const double variance =
      hands > 1 ? (sum_sq - (sum * sum) / static_cast<double>(hands)) / static_cast<double>(hands - 1)
                : 0.0;
  const double standard_error = std::sqrt(std::max(0.0, variance) / static_cast<double>(hands));

  SampledSelfplayReport report;
  report.hands = hands;
  report.utility_p0_mean = mean;
  report.utility_p1_mean = -mean;
  report.ci95 = 1.96 * standard_error;
  return report;
}

void WriteHandHistoryCsv(const std::string& output_path,
                         const std::vector<SampledHandRow>& rows,
                         const std::string& policy_a,
                         const std::string& policy_b,
                         std::uint64_t sample_seed) {
  std::ofstream out(output_path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("Failed to open hand history output: " + output_path);
  }

  out << "hand_id,delta_p0_bb,state_key,action_bucket,position,street,p0_card,p1_card,policy_a,policy_b,sample_seed\n";
  out << std::setprecision(12);

  for (const auto& row : rows) {
    out << row.hand_id << ','
        << row.delta_p0_bb << ','
        << EscapeCsv(row.state_key) << ','
        << EscapeCsv(row.action_bucket) << ','
        << EscapeCsv(row.position) << ','
        << EscapeCsv(row.street) << ','
        << EscapeCsv(row.p0_card) << ','
        << EscapeCsv(row.p1_card) << ','
        << EscapeCsv(policy_a) << ','
        << EscapeCsv(policy_b) << ','
        << sample_seed
        << '\n';
  }
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

    const auto exact_report =
        plarbius::eval::EvaluateExpectedSelfplay(*game, policy_a, policy_b, options.threads);

    std::cout << "game=" << options.game << '\n';
    std::cout << "policy_a=" << options.policy_a_path << '\n';
    std::cout << "policy_b=" << options.policy_b_path << '\n';
    std::cout << "threads=" << options.threads << '\n';
    std::cout << "utility_p0=" << std::fixed << std::setprecision(6) << exact_report.utility_p0 << '\n';
    std::cout << "utility_p1=" << std::fixed << std::setprecision(6) << exact_report.utility_p1 << '\n';
    std::cout << "evaluated_states=" << exact_report.evaluated_states << '\n';

    if (options.sampled_hands > 0) {
      std::vector<SampledHandRow> hand_rows;
      const auto sampled_report = EvaluateSampledSelfplay(
          *game,
          policy_a,
          policy_b,
          options.sampled_hands,
          options.sample_seed,
          &hand_rows);

      std::cout << "sampled_hands=" << sampled_report.hands << '\n';
      std::cout << "sample_seed=" << options.sample_seed << '\n';
      std::cout << "sampled_utility_mean_p0=" << std::fixed << std::setprecision(6)
                << sampled_report.utility_p0_mean << '\n';
      std::cout << "sampled_utility_mean_p1=" << std::fixed << std::setprecision(6)
                << sampled_report.utility_p1_mean << '\n';
      std::cout << "sampled_ci95=" << std::fixed << std::setprecision(6)
                << sampled_report.ci95 << '\n';

      if (!options.hand_history_out_path.empty()) {
        WriteHandHistoryCsv(
            options.hand_history_out_path,
            hand_rows,
            options.policy_a_path,
            options.policy_b_path,
            options.sample_seed);
        std::cout << "hand_history_out=" << options.hand_history_out_path << '\n';
      }
    }

    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
