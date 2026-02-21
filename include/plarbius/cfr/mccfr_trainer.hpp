#pragma once

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "plarbius/cfr/infoset_table.hpp"
#include "plarbius/cfr/training_config.hpp"
#include "plarbius/cfr/training_metrics.hpp"
#include "plarbius/core/types.hpp"
#include "plarbius/game/game.hpp"
#include "plarbius/infra/logger.hpp"

namespace plarbius::cfr {

class MccfrTrainer {
 public:
  MccfrTrainer(const game::Game& game,
               TrainerConfig config,
               std::shared_ptr<infra::Logger> logger = nullptr,
               TrainingMetricsCallback metrics_callback = nullptr);

  void Train();

  [[nodiscard]] const InfosetTable& Table() const noexcept;

 private:
  double Traverse(const game::GameState& state,
                  PlayerId update_player,
                  std::uint64_t iteration,
                  double reach_opponent_target,
                  double reach_sampling);
  std::size_t SampleIndex(const std::vector<double>& probs);
  std::vector<double> BuildSamplingDistribution(const std::vector<double>& strategy) const;

  const game::Game& game_;
  TrainerConfig config_;
  InfosetTable table_;
  std::shared_ptr<infra::Logger> logger_;
  TrainingMetricsCallback metrics_callback_;
  std::mt19937_64 rng_;
};

}  // namespace plarbius::cfr
