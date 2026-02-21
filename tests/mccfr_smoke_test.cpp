#include <memory>
#include <sstream>

#include "plarbius/cfr/mccfr_trainer.hpp"
#include "plarbius/games/kuhn/kuhn_game.hpp"
#include "plarbius/infra/logger.hpp"

int main() {
  std::ostringstream sink;
  auto logger = std::make_shared<plarbius::infra::Logger>(sink);

  plarbius::games::kuhn::KuhnGame game;
  plarbius::cfr::TrainerConfig config;
  config.iterations = 300;
  config.log_interval = 100;
  config.seed = 7;
  config.averaging_delay = 0;

  plarbius::cfr::MccfrTrainer trainer(game, config, logger);
  trainer.Train();

  if (trainer.Table().Size() == 0) {
    return 1;
  }

  return 0;
}

