#include <memory>
#include <sstream>

#include "plarbius/cfr/cfr_plus_trainer.hpp"
#include "plarbius/games/kuhn/kuhn_game.hpp"
#include "plarbius/infra/logger.hpp"

int main() {
  std::ostringstream sink;
  auto logger = std::make_shared<plarbius::infra::Logger>(sink);

  plarbius::games::kuhn::KuhnGame game;
  plarbius::cfr::TrainerConfig config;
  config.iterations = 200;
  config.log_interval = 100;
  config.averaging_delay = 0;

  plarbius::cfr::CfrPlusTrainer trainer(game, config, logger);
  trainer.Train();

  if (trainer.Table().Size() == 0) {
    return 1;
  }

  return 0;
}
