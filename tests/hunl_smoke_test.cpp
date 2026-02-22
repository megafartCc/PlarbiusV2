#include <memory>
#include <sstream>

#include "plarbius/cfr/mccfr_trainer.hpp"
#include "plarbius/games/hunl/hunl_game.hpp"
#include "plarbius/infra/logger.hpp"
#include "plarbius/policy/policy_io.hpp"

int main() {
  plarbius::games::hunl::HunlGameConfig game_config = plarbius::games::hunl::DefaultHunlGameConfig();
  game_config.bucket_config.chance_outcomes = 8;
  game_config.bucket_config.preflop_private_buckets = 6;
  game_config.bucket_config.flop_private_buckets = 8;
  game_config.bucket_config.turn_private_buckets = 8;
  game_config.bucket_config.river_private_buckets = 8;
  game_config.bucket_config.flop_public_buckets = 4;
  game_config.bucket_config.turn_public_buckets = 4;
  game_config.bucket_config.river_public_buckets = 4;
  game_config.action_config.preflop_raise_sizes_bb = {2.0, 6.0};
  game_config.action_config.postflop_bet_sizes_pot = {0.5, 1.0};
  game_config.action_config.postflop_raise_sizes_pot = {1.0};

  std::ostringstream sink;
  auto logger = std::make_shared<plarbius::infra::Logger>(sink);

  plarbius::games::hunl::HunlGame game(game_config);

  plarbius::cfr::TrainerConfig train_config;
  train_config.iterations = 40;
  train_config.log_interval = 40;
  train_config.averaging_delay = 0;
  train_config.seed = 13;
  train_config.sampling_epsilon = 0.2;

  plarbius::cfr::MccfrTrainer trainer(game, train_config, logger);
  trainer.Train();

  if (trainer.Table().Size() == 0) {
    return 1;
  }

  const auto policy = plarbius::policy::BuildAveragePolicy(trainer.Table());
  if (policy.empty()) {
    return 1;
  }

  return 0;
}
