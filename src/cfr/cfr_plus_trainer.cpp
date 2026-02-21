#include "plarbius/cfr/cfr_plus_trainer.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "plarbius/cfr/infoset_checkpoint_io.hpp"
#include "plarbius/cfr/regret_matcher.hpp"

namespace plarbius::cfr {

CfrPlusTrainer::CfrPlusTrainer(const game::Game& game,
                               TrainerConfig config,
                               std::shared_ptr<infra::Logger> logger,
                               TrainingMetricsCallback metrics_callback)
    : game_(game),
      config_(config),
      logger_(std::move(logger)),
      metrics_callback_(std::move(metrics_callback)) {
  if (logger_ == nullptr) {
    logger_ = std::make_shared<infra::Logger>(std::cout);
  }
}

void CfrPlusTrainer::Train() {
  const auto start_time = std::chrono::steady_clock::now();

  if (!config_.resume_path.empty()) {
    InfosetCheckpointIo::Load(table_, config_.resume_path);
    std::ostringstream loaded;
    loaded << "Loaded checkpoint from " << config_.resume_path << ", infosets=" << table_.Size();
    logger_->Info(loaded.str());
  }

  std::ostringstream start;
  start << "Starting CFR+ on " << game_.Name() << " for " << config_.iterations << " iterations.";
  logger_->Info(start.str());

  for (std::uint64_t iteration = 1; iteration <= config_.iterations; ++iteration) {
    for (std::size_t player = 0; player < kNumPlayers; ++player) {
      const auto update_player = static_cast<PlayerId>(player);
      auto state = game_.NewInitialState();
      Traverse(*state, update_player, 1.0, 1.0, iteration);
    }

    if (config_.log_interval > 0 && (iteration % config_.log_interval) == 0) {
      std::ostringstream progress;
      progress << "Iteration " << iteration << ", infosets=" << table_.Size();
      logger_->Info(progress.str());
    }

    if (config_.checkpoint_every > 0 && !config_.checkpoint_path.empty() &&
        (iteration % config_.checkpoint_every) == 0) {
      InfosetCheckpointIo::Save(table_, config_.checkpoint_path);
      std::ostringstream saved;
      saved << "Checkpoint saved at iteration " << iteration << " to " << config_.checkpoint_path;
      logger_->Info(saved.str());
    }

    if (metrics_callback_ != nullptr && config_.metrics_interval > 0 &&
        (iteration % config_.metrics_interval) == 0) {
      metrics_callback_(iteration, table_);
    }
  }

  if (!config_.checkpoint_path.empty()) {
    InfosetCheckpointIo::Save(table_, config_.checkpoint_path);
    std::ostringstream saved;
    saved << "Final checkpoint saved to " << config_.checkpoint_path;
    logger_->Info(saved.str());
  }

  if (metrics_callback_ != nullptr) {
    metrics_callback_(config_.iterations, table_);
  }

  const auto end_time = std::chrono::steady_clock::now();
  const double elapsed_seconds =
      std::chrono::duration<double>(end_time - start_time).count();
  if (elapsed_seconds > 0.0) {
    std::ostringstream perf;
    perf << "Profile: elapsed=" << elapsed_seconds << "s, iterations/s="
         << (static_cast<double>(config_.iterations) / elapsed_seconds) << ", infosets="
         << table_.Size();
    logger_->Info(perf.str());
  }

  logger_->Info("Training completed.");
}

const InfosetTable& CfrPlusTrainer::Table() const noexcept {
  return table_;
}

double CfrPlusTrainer::Traverse(const game::GameState& state,
                                PlayerId update_player,
                                double reach_update,
                                double reach_opponent,
                                std::uint64_t iteration) {
  if (state.IsTerminal()) {
    return state.TerminalUtility(update_player);
  }

  if (state.IsChanceNode()) {
    double expected = 0.0;
    for (const auto& outcome : state.ChanceOutcomes()) {
      const auto next = state.CloneAndApplyChance(outcome.id);
      expected += outcome.probability *
                  Traverse(*next, update_player, reach_update, reach_opponent, iteration);
    }
    return expected;
  }

  const auto actions = state.LegalActions();
  if (actions.empty()) {
    throw std::runtime_error("Non-terminal decision node has no legal actions.");
  }

  const PlayerId current_player = state.CurrentPlayer();
  const std::string key = state.InfosetKey(current_player);
  InfosetNode& node = table_.GetOrCreate(key, actions.size());
  const auto strategy = RegretMatcher::ComputeStrategyPlus(node.regret_sum);

  const double average_weight = iteration > config_.averaging_delay
                                    ? static_cast<double>(iteration - config_.averaging_delay)
                                    : 1.0;
  const double reach_for_average =
      current_player == update_player ? reach_update : reach_opponent;
  for (std::size_t i = 0; i < strategy.size(); ++i) {
    node.strategy_sum[i] += average_weight * reach_for_average * strategy[i];
  }

  std::vector<double> action_utilities(actions.size(), 0.0);
  double node_utility = 0.0;

  for (std::size_t i = 0; i < actions.size(); ++i) {
    const auto next = state.CloneAndApplyAction(actions[i]);
    if (current_player == update_player) {
      action_utilities[i] =
          Traverse(*next, update_player, reach_update * strategy[i], reach_opponent, iteration);
    } else {
      action_utilities[i] =
          Traverse(*next, update_player, reach_update, reach_opponent * strategy[i], iteration);
    }
    node_utility += strategy[i] * action_utilities[i];
  }

  if (current_player == update_player) {
    for (std::size_t i = 0; i < actions.size(); ++i) {
      const double regret = reach_opponent * (action_utilities[i] - node_utility);
      node.regret_sum[i] = std::max(0.0, node.regret_sum[i] + regret);
    }
  }

  return node_utility;
}

}  // namespace plarbius::cfr
