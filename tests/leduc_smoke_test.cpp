#include <memory>
#include <sstream>

#include "plarbius/cfr/cfr_plus_trainer.hpp"
#include "plarbius/cfr/mccfr_trainer.hpp"
#include "plarbius/eval/selfplay_evaluator.hpp"
#include "plarbius/games/leduc/leduc_game.hpp"
#include "plarbius/infra/logger.hpp"
#include "plarbius/policy/policy_io.hpp"

int main() {
  std::ostringstream sink;
  auto logger = std::make_shared<plarbius::infra::Logger>(sink);

  plarbius::games::leduc::LeducGame game;
  plarbius::cfr::TrainerConfig cfr_plus_config;
  cfr_plus_config.iterations = 150;
  cfr_plus_config.log_interval = 150;
  cfr_plus_config.averaging_delay = 0;

  plarbius::cfr::CfrPlusTrainer cfr_plus(game, cfr_plus_config, logger);
  cfr_plus.Train();
  if (cfr_plus.Table().Size() == 0) {
    return 1;
  }

  plarbius::cfr::TrainerConfig mccfr_config;
  mccfr_config.iterations = 150;
  mccfr_config.log_interval = 150;
  mccfr_config.averaging_delay = 0;
  mccfr_config.seed = 11;
  mccfr_config.sampling_epsilon = 0.3;

  plarbius::cfr::MccfrTrainer mccfr(game, mccfr_config, logger);
  mccfr.Train();
  if (mccfr.Table().Size() == 0) {
    return 1;
  }

  const auto policy = plarbius::policy::BuildAveragePolicy(cfr_plus.Table());
  const auto report = plarbius::eval::EvaluateExpectedSelfplay(game, policy, policy, 2);
  if (report.evaluated_states == 0) {
    return 1;
  }

  return 0;
}

