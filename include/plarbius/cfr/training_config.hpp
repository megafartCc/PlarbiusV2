#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace plarbius::cfr {

struct TrainerConfig {
  std::uint64_t iterations = 50000;
  std::uint64_t log_interval = 5000;
  std::uint64_t averaging_delay = 0;
  std::uint64_t checkpoint_every = 0;
  std::uint64_t seed = 1;
  std::uint64_t metrics_interval = 0;
  double sampling_epsilon = 0.0;
  std::size_t num_threads = 1;

  bool mccfr_use_lcfr_discount = false;
  std::uint64_t mccfr_lcfr_discount_start = 1;
  std::uint64_t mccfr_lcfr_discount_interval = 1;
  bool mccfr_lcfr_discount_strategy_sum = true;

  bool mccfr_enable_pruning = false;
  std::uint64_t mccfr_prune_start = 1;
  std::uint64_t mccfr_prune_full_traversal_interval = 0;
  std::size_t mccfr_prune_min_actions = 2;
  double mccfr_prune_regret_threshold = 1e-6;

  std::string checkpoint_path;
  std::string resume_path;
};

}  // namespace plarbius::cfr
