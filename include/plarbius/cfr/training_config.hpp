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
  std::string checkpoint_path;
  std::string resume_path;
};

}  // namespace plarbius::cfr
