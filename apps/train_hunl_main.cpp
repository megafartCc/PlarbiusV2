#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "plarbius/cfr/cfr_plus_trainer.hpp"
#include "plarbius/cfr/mccfr_trainer.hpp"
#include "plarbius/cfr/regret_matcher.hpp"
#include "plarbius/cfr/training_config.hpp"
#include "plarbius/cfr/training_metrics.hpp"
#include "plarbius/games/hunl/hunl_action_abstraction.hpp"
#include "plarbius/games/hunl/hunl_bucket_config.hpp"
#include "plarbius/games/hunl/hunl_game.hpp"
#include "plarbius/policy/policy_io.hpp"

namespace {

struct CliOptions {
  std::string algo = "mccfr";
  std::string metrics_out_path;
  std::string policy_out_path;
  std::string bucket_config_path;
  std::string action_config_path;
  bool print_strategy = false;
  std::size_t strategy_limit = 32;
  plarbius::cfr::TrainerConfig trainer_config;
  plarbius::games::hunl::HunlGameConfig game_config;
};

class HunlMetricsCsvWriter {
 public:
  HunlMetricsCsvWriter(std::string path,
                       std::string algo,
                       std::uint64_t seed)
      : path_(std::move(path)), algo_(std::move(algo)), seed_(seed) {
    out_.open(path_, std::ios::trunc);
    if (!out_) {
      throw std::runtime_error("Failed to open metrics file: " + path_);
    }
    out_ << "algorithm,game,seed,iteration,infosets,utility_p0,utility_p1,best_response_p0,"
            "best_response_p1,nash_conv,exploitability\n";
  }

  void Write(std::uint64_t iteration, const plarbius::cfr::InfosetTable& table) {
    if (iteration == last_iteration_) {
      return;
    }
    out_ << algo_ << ",hunl," << seed_ << ',' << iteration << ',' << table.Size() << ",,,,,,\n";
    last_iteration_ = iteration;
    out_.flush();
  }

 private:
  std::string path_;
  std::string algo_;
  std::uint64_t seed_;
  std::ofstream out_;
  std::uint64_t last_iteration_ = 0;
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

[[nodiscard]] double ParseDouble(const std::string& text, const char* name) {
  try {
    return std::stod(text);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string("Invalid value for ") + name + ": " + text);
  }
}

void PrintUsage() {
  std::cout << "Usage:\n"
            << "  plarbius_train_hunl [iterations] [--algo cfr+|mccfr] [--seed N]\n"
            << "                      [--bucket-config path] [--action-config path]\n"
            << "                      [--stack-bb X] [--small-blind-bb X] [--big-blind-bb X]\n"
            << "                      [--sampling-epsilon X]\n"
            << "                      [--lcfr-discount] [--lcfr-start N] [--lcfr-interval N]\n"
            << "                      [--lcfr-no-strategy-discount]\n"
            << "                      [--prune-actions] [--prune-start N] [--prune-threshold X]\n"
            << "                      [--prune-min-actions N] [--prune-full-interval N]\n"
            << "                      [--checkpoint path] [--checkpoint-every N] [--resume path]\n"
            << "                      [--log-interval N] [--avg-delay N]\n"
            << "                      [--metrics-out path] [--metrics-interval N]\n"
            << "                      [--policy-out path] [--strategy-limit N] [--no-strategy-print]\n";
}

void PrintHunlConfigSummary(const plarbius::games::hunl::HunlGameConfig& config) {
  std::cout << "HUNL abstraction config\n";
  std::cout << "stack_bb=" << config.stack_bb
            << " blinds=" << config.small_blind_bb << "/" << config.big_blind_bb << '\n';
  std::cout << "buckets private preflop/flop/turn/river="
            << config.bucket_config.preflop_private_buckets << '/'
            << config.bucket_config.flop_private_buckets << '/'
            << config.bucket_config.turn_private_buckets << '/'
            << config.bucket_config.river_private_buckets << '\n';
  std::cout << "buckets public flop/turn/river="
            << config.bucket_config.flop_public_buckets << '/'
            << config.bucket_config.turn_public_buckets << '/'
            << config.bucket_config.river_public_buckets
            << " chance_outcomes=" << config.bucket_config.chance_outcomes << '\n';
  std::cout << "action abstraction raises_per_round="
            << config.action_config.max_raises_per_round
            << " all_in=" << (config.action_config.allow_all_in ? "true" : "false")
            << " min_bet_bb=" << config.action_config.min_bet_bb << '\n';
}

void PrintStrategySnapshot(const plarbius::cfr::InfosetTable& table, std::size_t strategy_limit) {
  std::vector<std::pair<std::string, plarbius::cfr::InfosetNode>> snapshot(
      table.Nodes().begin(),
      table.Nodes().end());
  std::sort(snapshot.begin(), snapshot.end(), [](const auto& left, const auto& right) {
    return left.first < right.first;
  });

  std::cout << "\nAverage strategy snapshot\n";
  std::cout << "infosets=" << snapshot.size() << '\n';
  const std::size_t limit = std::min(strategy_limit, snapshot.size());
  for (std::size_t row = 0; row < limit; ++row) {
    const auto& key = snapshot[row].first;
    const auto& node = snapshot[row].second;
    const auto avg = plarbius::cfr::RegretMatcher::Normalize(node.strategy_sum);
    std::cout << key;
    for (double p : avg) {
      std::cout << ' ' << std::fixed << std::setprecision(3) << p;
    }
    std::cout << '\n';
  }
  if (snapshot.size() > limit) {
    std::cout << "... truncated " << (snapshot.size() - limit) << " infosets\n";
  }
}

CliOptions ParseCli(int argc, char** argv) {
  CliOptions options;
  options.trainer_config.iterations = 50000;

  int index = 1;
  if (argc > 1 && !LooksLikeFlag(argv[1])) {
    options.trainer_config.iterations = ParseUnsigned(argv[1], "iterations");
    index = 2;
  }

  while (index < argc) {
    const std::string flag = argv[index++];
    if (flag == "--algo") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --algo.");
      }
      options.algo = argv[index++];
    } else if (flag == "--seed") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --seed.");
      }
      options.trainer_config.seed = ParseUnsigned(argv[index++], "--seed");
    } else if (flag == "--bucket-config") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --bucket-config.");
      }
      options.bucket_config_path = argv[index++];
    } else if (flag == "--action-config") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --action-config.");
      }
      options.action_config_path = argv[index++];
    } else if (flag == "--stack-bb") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --stack-bb.");
      }
      options.game_config.stack_bb = ParseDouble(argv[index++], "--stack-bb");
    } else if (flag == "--small-blind-bb") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --small-blind-bb.");
      }
      options.game_config.small_blind_bb = ParseDouble(argv[index++], "--small-blind-bb");
    } else if (flag == "--big-blind-bb") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --big-blind-bb.");
      }
      options.game_config.big_blind_bb = ParseDouble(argv[index++], "--big-blind-bb");
    } else if (flag == "--sampling-epsilon") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --sampling-epsilon.");
      }
      options.trainer_config.sampling_epsilon = ParseDouble(argv[index++], "--sampling-epsilon");
    } else if (flag == "--lcfr-discount") {
      options.trainer_config.mccfr_use_lcfr_discount = true;
    } else if (flag == "--lcfr-start") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --lcfr-start.");
      }
      options.trainer_config.mccfr_lcfr_discount_start = ParseUnsigned(argv[index++], "--lcfr-start");
    } else if (flag == "--lcfr-interval") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --lcfr-interval.");
      }
      options.trainer_config.mccfr_lcfr_discount_interval =
          ParseUnsigned(argv[index++], "--lcfr-interval");
    } else if (flag == "--lcfr-no-strategy-discount") {
      options.trainer_config.mccfr_lcfr_discount_strategy_sum = false;
    } else if (flag == "--prune-actions") {
      options.trainer_config.mccfr_enable_pruning = true;
    } else if (flag == "--prune-start") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --prune-start.");
      }
      options.trainer_config.mccfr_prune_start = ParseUnsigned(argv[index++], "--prune-start");
    } else if (flag == "--prune-threshold") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --prune-threshold.");
      }
      options.trainer_config.mccfr_prune_regret_threshold =
          ParseDouble(argv[index++], "--prune-threshold");
    } else if (flag == "--prune-min-actions") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --prune-min-actions.");
      }
      options.trainer_config.mccfr_prune_min_actions =
          static_cast<std::size_t>(ParseUnsigned(argv[index++], "--prune-min-actions"));
    } else if (flag == "--prune-full-interval") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --prune-full-interval.");
      }
      options.trainer_config.mccfr_prune_full_traversal_interval =
          ParseUnsigned(argv[index++], "--prune-full-interval");
    } else if (flag == "--checkpoint") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --checkpoint.");
      }
      options.trainer_config.checkpoint_path = argv[index++];
    } else if (flag == "--checkpoint-every") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --checkpoint-every.");
      }
      options.trainer_config.checkpoint_every = ParseUnsigned(argv[index++], "--checkpoint-every");
    } else if (flag == "--resume") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --resume.");
      }
      options.trainer_config.resume_path = argv[index++];
    } else if (flag == "--log-interval") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --log-interval.");
      }
      options.trainer_config.log_interval = ParseUnsigned(argv[index++], "--log-interval");
    } else if (flag == "--avg-delay") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --avg-delay.");
      }
      options.trainer_config.averaging_delay = ParseUnsigned(argv[index++], "--avg-delay");
    } else if (flag == "--metrics-out") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --metrics-out.");
      }
      options.metrics_out_path = argv[index++];
    } else if (flag == "--metrics-interval") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --metrics-interval.");
      }
      options.trainer_config.metrics_interval = ParseUnsigned(argv[index++], "--metrics-interval");
    } else if (flag == "--policy-out") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --policy-out.");
      }
      options.policy_out_path = argv[index++];
    } else if (flag == "--strategy-limit") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --strategy-limit.");
      }
      options.strategy_limit = static_cast<std::size_t>(ParseUnsigned(argv[index++], "--strategy-limit"));
    } else if (flag == "--no-strategy-print") {
      options.print_strategy = false;
    } else if (flag == "--strategy-print") {
      options.print_strategy = true;
    } else if (flag == "--help" || flag == "-h") {
      PrintUsage();
      std::exit(0);
    } else {
      throw std::invalid_argument("Unknown flag: " + flag);
    }
  }

  if (!options.bucket_config_path.empty()) {
    options.game_config.bucket_config =
        plarbius::games::hunl::LoadHunlBucketConfig(options.bucket_config_path);
  }
  if (!options.action_config_path.empty()) {
    options.game_config.action_config =
        plarbius::games::hunl::LoadHunlActionAbstractionConfig(options.action_config_path);
  }

  if (options.trainer_config.iterations == 0) {
    throw std::invalid_argument("iterations must be > 0.");
  }
  if (options.trainer_config.log_interval == 0) {
    options.trainer_config.log_interval =
        std::max<std::uint64_t>(1, options.trainer_config.iterations / 20);
  }
  if (options.trainer_config.averaging_delay == 0) {
    options.trainer_config.averaging_delay = options.trainer_config.iterations / 20;
  }
  if (options.trainer_config.metrics_interval == 0 && !options.metrics_out_path.empty()) {
    options.trainer_config.metrics_interval =
        options.trainer_config.checkpoint_every > 0
            ? options.trainer_config.checkpoint_every
            : options.trainer_config.log_interval;
  }
  if (options.trainer_config.mccfr_lcfr_discount_interval == 0) {
    options.trainer_config.mccfr_lcfr_discount_interval = 1;
  }
  if (options.trainer_config.mccfr_prune_min_actions == 0) {
    options.trainer_config.mccfr_prune_min_actions = 1;
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
    plarbius::games::hunl::ValidateHunlGameConfig(options.game_config);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }

  PrintHunlConfigSummary(options.game_config);

  if (options.algo == "cfr+" && options.game_config.bucket_config.chance_outcomes > 16) {
    std::cerr << "cfr+ on this HUNL scaffold is too expensive with chance_outcomes="
              << options.game_config.bucket_config.chance_outcomes
              << ". Use --algo mccfr or reduce chance_outcomes to <=16 in bucket config.\n";
    return 1;
  }

  plarbius::games::hunl::HunlGame game(options.game_config);

  std::shared_ptr<HunlMetricsCsvWriter> metrics_writer;
  plarbius::cfr::TrainingMetricsCallback metrics_callback = nullptr;
  if (!options.metrics_out_path.empty()) {
    metrics_writer = std::make_shared<HunlMetricsCsvWriter>(
        options.metrics_out_path, options.algo, options.trainer_config.seed);
    metrics_callback = [metrics_writer](std::uint64_t iteration,
                                        const plarbius::cfr::InfosetTable& table) {
      metrics_writer->Write(iteration, table);
    };
  }

  plarbius::cfr::InfosetTable result_table;
  if (options.algo == "cfr+") {
    plarbius::cfr::CfrPlusTrainer trainer(game, options.trainer_config, nullptr, metrics_callback);
    trainer.Train();
    result_table = trainer.Table();
  } else if (options.algo == "mccfr") {
    plarbius::cfr::MccfrTrainer trainer(game, options.trainer_config, nullptr, metrics_callback);
    trainer.Train();
    result_table = trainer.Table();
  } else {
    std::cerr << "Unsupported algorithm: " << options.algo << '\n';
    return 1;
  }

  if (options.print_strategy) {
    PrintStrategySnapshot(result_table, options.strategy_limit);
  }

  if (!options.policy_out_path.empty()) {
    auto avg_policy = plarbius::policy::BuildAveragePolicy(result_table);
    plarbius::policy::SavePolicy(avg_policy, options.policy_out_path);
    std::cout << "policy_out=" << options.policy_out_path << '\n';
  }

  std::cout << "\nHUNL scaffold training complete.\n";
  std::cout << "infosets=" << result_table.Size() << '\n';
  std::cout << "note=exploitability evaluator is not yet implemented for hunl abstraction.\n";
  return 0;
}
