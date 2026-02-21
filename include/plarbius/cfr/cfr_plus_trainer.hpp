#pragma once

#include <cstdint>
#include <memory>

#include "plarbius/cfr/infoset_table.hpp"
#include "plarbius/cfr/training_config.hpp"
#include "plarbius/cfr/training_metrics.hpp"
#include "plarbius/core/types.hpp"
#include "plarbius/game/game.hpp"
#include "plarbius/infra/logger.hpp"

namespace plarbius::cfr {

class CfrPlusTrainer {
 public:
  CfrPlusTrainer(const game::Game& game,
                 TrainerConfig config,
                 std::shared_ptr<infra::Logger> logger = nullptr,
                 TrainingMetricsCallback metrics_callback = nullptr);

  void Train();

  [[nodiscard]] const InfosetTable& Table() const noexcept;

 private:
  double Traverse(const game::GameState& state,
                  PlayerId update_player,
                  double reach_update,
                  double reach_opponent,
                  std::uint64_t iteration);

  const game::Game& game_;
  TrainerConfig config_;
  InfosetTable table_;
  std::shared_ptr<infra::Logger> logger_;
  TrainingMetricsCallback metrics_callback_;
};

}  // namespace plarbius::cfr
