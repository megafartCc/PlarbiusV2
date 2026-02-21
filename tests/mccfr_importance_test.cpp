#include <cmath>
#include <memory>
#include <sstream>

#include "plarbius/cfr/mccfr_trainer.hpp"
#include "plarbius/eval/kuhn_exploitability.hpp"
#include "plarbius/games/kuhn/kuhn_game.hpp"
#include "plarbius/infra/logger.hpp"

namespace {

bool NearlyEqual(double a, double b, double epsilon = 1e-10) {
  return std::fabs(a - b) <= epsilon;
}

}  // namespace

int main() {
  std::ostringstream sink;
  auto logger = std::make_shared<plarbius::infra::Logger>(sink);

  plarbius::games::kuhn::KuhnGame game;
  plarbius::cfr::TrainerConfig config;
  config.iterations = 4000;
  config.log_interval = 4000;
  config.averaging_delay = 200;
  config.seed = 23;
  config.sampling_epsilon = 0.4;

  plarbius::cfr::MccfrTrainer trainer_a(game, config, logger);
  trainer_a.Train();

  plarbius::cfr::MccfrTrainer trainer_b(game, config, logger);
  trainer_b.Train();

  if (trainer_a.Table().Size() != trainer_b.Table().Size()) {
    return 1;
  }

  for (const auto& [key, node_a] : trainer_a.Table().Nodes()) {
    const auto it = trainer_b.Table().Nodes().find(key);
    if (it == trainer_b.Table().Nodes().end()) {
      return 1;
    }
    if (node_a.regret_sum.size() != it->second.regret_sum.size() ||
        node_a.strategy_sum.size() != it->second.strategy_sum.size()) {
      return 1;
    }
    for (std::size_t i = 0; i < node_a.regret_sum.size(); ++i) {
      if (!NearlyEqual(node_a.regret_sum[i], it->second.regret_sum[i])) {
        return 1;
      }
      if (!NearlyEqual(node_a.strategy_sum[i], it->second.strategy_sum[i])) {
        return 1;
      }
    }
  }

  const auto report = plarbius::eval::EvaluateKuhnExploitability(trainer_a.Table());
  if (report.exploitability > 0.10 || report.exploitability < 0.0 || !std::isfinite(report.exploitability)) {
    return 1;
  }

  return 0;
}

