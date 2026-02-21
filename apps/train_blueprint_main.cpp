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
#include "plarbius/eval/kuhn_exploitability.hpp"
#include "plarbius/game/game.hpp"
#include "plarbius/games/kuhn/kuhn_game.hpp"
#include "plarbius/games/leduc/leduc_game.hpp"
#include "plarbius/policy/policy_io.hpp"

namespace {

struct CliOptions {
  std::string algo = "cfr+";
  std::string game = "kuhn";
  std::string policy_out_path;
  std::string metrics_out_path;
  bool print_strategy = true;
  std::size_t strategy_limit = 32;
  plarbius::cfr::TrainerConfig config;
};

class KuhnMetricsCsvWriter {
 public:
  KuhnMetricsCsvWriter(std::string path,
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
    const auto report = plarbius::eval::EvaluateKuhnExploitability(table);
    out_ << algo_ << ",kuhn," << seed_ << ',' << iteration << ',' << table.Size() << ','
         << report.utility_p0 << ',' << report.utility_p1 << ',' << report.best_response_p0 << ','
         << report.best_response_p1 << ',' << report.nash_conv << ',' << report.exploitability
         << '\n';
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
            << "  plarbius_train [iterations] [--algo cfr+|mccfr] [--game kuhn|leduc]\n"
            << "                 [--seed N] [--sampling-epsilon X]\n"
            << "                 [--checkpoint path] [--checkpoint-every N] [--resume path]\n"
            << "                 [--log-interval N] [--avg-delay N]\n"
            << "                 [--metrics-out path] [--metrics-interval N]\n"
            << "                 [--policy-out path] [--strategy-limit N] [--no-strategy-print]\n";
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
  options.config.iterations = 50000;

  int index = 1;
  if (argc > 1 && !LooksLikeFlag(argv[1])) {
    options.config.iterations = ParseUnsigned(argv[1], "iterations");
    index = 2;
  }

  while (index < argc) {
    const std::string flag = argv[index++];
    if (flag == "--algo") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --algo.");
      }
      options.algo = argv[index++];
    } else if (flag == "--game") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --game.");
      }
      options.game = argv[index++];
    } else if (flag == "--seed") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --seed.");
      }
      options.config.seed = ParseUnsigned(argv[index++], "--seed");
    } else if (flag == "--sampling-epsilon") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --sampling-epsilon.");
      }
      options.config.sampling_epsilon = ParseDouble(argv[index++], "--sampling-epsilon");
    } else if (flag == "--checkpoint") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --checkpoint.");
      }
      options.config.checkpoint_path = argv[index++];
    } else if (flag == "--checkpoint-every") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --checkpoint-every.");
      }
      options.config.checkpoint_every = ParseUnsigned(argv[index++], "--checkpoint-every");
    } else if (flag == "--resume") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --resume.");
      }
      options.config.resume_path = argv[index++];
    } else if (flag == "--log-interval") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --log-interval.");
      }
      options.config.log_interval = ParseUnsigned(argv[index++], "--log-interval");
    } else if (flag == "--avg-delay") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --avg-delay.");
      }
      options.config.averaging_delay = ParseUnsigned(argv[index++], "--avg-delay");
    } else if (flag == "--metrics-out") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --metrics-out.");
      }
      options.metrics_out_path = argv[index++];
    } else if (flag == "--metrics-interval") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --metrics-interval.");
      }
      options.config.metrics_interval = ParseUnsigned(argv[index++], "--metrics-interval");
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
    } else if (flag == "--help" || flag == "-h") {
      PrintUsage();
      std::exit(0);
    } else {
      throw std::invalid_argument("Unknown flag: " + flag);
    }
  }

  if (options.config.iterations == 0) {
    throw std::invalid_argument("iterations must be > 0.");
  }
  if (options.config.log_interval == 0) {
    options.config.log_interval = std::max<std::uint64_t>(1, options.config.iterations / 10);
  }
  if (options.config.averaging_delay == 0) {
    options.config.averaging_delay = options.config.iterations / 20;
  }
  if (options.config.metrics_interval == 0 && !options.metrics_out_path.empty()) {
    options.config.metrics_interval = options.config.checkpoint_every > 0
                                          ? options.config.checkpoint_every
                                          : options.config.log_interval;
  }

  return options;
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

  std::unique_ptr<plarbius::game::Game> game;
  try {
    game = MakeGame(options.game);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }

  std::shared_ptr<KuhnMetricsCsvWriter> metrics_writer;
  plarbius::cfr::TrainingMetricsCallback metrics_callback = nullptr;
  if (!options.metrics_out_path.empty()) {
    if (options.game != "kuhn") {
      std::cerr << "--metrics-out currently supports only --game kuhn.\n";
      return 1;
    }
    metrics_writer =
        std::make_shared<KuhnMetricsCsvWriter>(options.metrics_out_path, options.algo, options.config.seed);
    metrics_callback = [metrics_writer](std::uint64_t iteration,
                                        const plarbius::cfr::InfosetTable& table) {
      metrics_writer->Write(iteration, table);
    };
  }

  plarbius::cfr::InfosetTable result_table;

  if (options.algo == "cfr+") {
    plarbius::cfr::CfrPlusTrainer trainer(*game, options.config, nullptr, metrics_callback);
    trainer.Train();
    result_table = trainer.Table();
  } else if (options.algo == "mccfr") {
    plarbius::cfr::MccfrTrainer trainer(*game, options.config, nullptr, metrics_callback);
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

  if (options.game == "kuhn") {
    const auto report = plarbius::eval::EvaluateKuhnExploitability(result_table);
    std::cout << "\nKuhn exploitability report\n";
    std::cout << "utility_p0=" << std::fixed << std::setprecision(6) << report.utility_p0 << '\n';
    std::cout << "utility_p1=" << std::fixed << std::setprecision(6) << report.utility_p1 << '\n';
    std::cout << "best_response_p0=" << std::fixed << std::setprecision(6) << report.best_response_p0
              << '\n';
    std::cout << "best_response_p1=" << std::fixed << std::setprecision(6) << report.best_response_p1
              << '\n';
    std::cout << "nash_conv=" << std::fixed << std::setprecision(6) << report.nash_conv << '\n';
    std::cout << "exploitability=" << std::fixed << std::setprecision(6) << report.exploitability
              << '\n';
  } else {
    std::cout << "\nExploitability evaluator is currently implemented for Kuhn only.\n";
  }

  return 0;
}

