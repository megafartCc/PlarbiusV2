#include "plarbius/cfr/mccfr_trainer.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "plarbius/cfr/infoset_checkpoint_io.hpp"
#include "plarbius/cfr/regret_matcher.hpp"

namespace plarbius::cfr {

MccfrTrainer::MccfrTrainer(const game::Game& game,
                           TrainerConfig config,
                           std::shared_ptr<infra::Logger> logger,
                           TrainingMetricsCallback metrics_callback)
    : game_(game),
      config_(std::move(config)),
      logger_(std::move(logger)),
      metrics_callback_(std::move(metrics_callback)),
      rng_(config_.seed) {
  if (logger_ == nullptr) {
    logger_ = std::make_shared<infra::Logger>(std::cout);
  }
}

void MccfrTrainer::Train() {
  const auto start_time = std::chrono::steady_clock::now();

  if (!config_.resume_path.empty()) {
    InfosetCheckpointIo::Load(table_, config_.resume_path);
    std::ostringstream loaded;
    loaded << "Loaded checkpoint from " << config_.resume_path << ", infosets=" << table_.Size();
    logger_->Info(loaded.str());
  }

  std::ostringstream start;
  start << "Starting external-sampling MCCFR on " << game_.Name() << " for " << config_.iterations
        << " iterations with seed=" << config_.seed
        << " and sampling_epsilon=" << config_.sampling_epsilon << '.';
  if (config_.mccfr_use_lcfr_discount) {
    start << " lcfr_discount(start=" << config_.mccfr_lcfr_discount_start
          << ", interval=" << config_.mccfr_lcfr_discount_interval
          << ", strategy_sum=" << (config_.mccfr_lcfr_discount_strategy_sum ? "on" : "off")
          << ").";
  }
  if (config_.mccfr_enable_pruning) {
    start << " pruning(start=" << config_.mccfr_prune_start
          << ", threshold=" << config_.mccfr_prune_regret_threshold
          << ", min_actions=" << config_.mccfr_prune_min_actions
          << ", full_interval=" << config_.mccfr_prune_full_traversal_interval
          << ").";
  }
  logger_->Info(start.str());

  for (std::uint64_t iteration = 1; iteration <= config_.iterations; ++iteration) {
    ApplyLcfrDiscount(iteration);
    for (std::size_t player = 0; player < kNumPlayers; ++player) {
      auto state = game_.NewInitialState();
      Traverse(*state, static_cast<PlayerId>(player), iteration, 1.0, 1.0);
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
  const double elapsed_seconds = std::chrono::duration<double>(end_time - start_time).count();
  if (elapsed_seconds > 0.0) {
    std::ostringstream perf;
    perf << "Profile: elapsed=" << elapsed_seconds << "s, iterations/s="
         << (static_cast<double>(config_.iterations) / elapsed_seconds) << ", infosets="
         << table_.Size();
    logger_->Info(perf.str());
  }

  logger_->Info("Training completed.");
}

const InfosetTable& MccfrTrainer::Table() const noexcept {
  return table_;
}

void MccfrTrainer::ApplyLcfrDiscount(std::uint64_t iteration) {
  if (!config_.mccfr_use_lcfr_discount) {
    return;
  }
  if (iteration < config_.mccfr_lcfr_discount_start) {
    return;
  }
  if (config_.mccfr_lcfr_discount_interval > 1 &&
      (iteration % config_.mccfr_lcfr_discount_interval) != 0) {
    return;
  }

  const double scale =
      static_cast<double>(iteration) / static_cast<double>(iteration + 1ULL);
  for (auto& [key, node] : table_.MutableNodes()) {
    (void)key;
    for (double& regret : node.regret_sum) {
      regret *= scale;
    }
    if (config_.mccfr_lcfr_discount_strategy_sum) {
      for (double& strategy : node.strategy_sum) {
        strategy *= scale;
      }
    }
  }
}

std::vector<bool> MccfrTrainer::BuildPrunedActionMask(const InfosetNode& node,
                                                      std::size_t action_count,
                                                      std::uint64_t iteration) const {
  std::vector<bool> mask(action_count, true);
  const std::size_t min_actions =
      std::min<std::size_t>(action_count, std::max<std::size_t>(1, config_.mccfr_prune_min_actions));
  if (!config_.mccfr_enable_pruning || action_count <= min_actions) {
    return mask;
  }
  if (iteration < config_.mccfr_prune_start) {
    return mask;
  }
  if (config_.mccfr_prune_full_traversal_interval > 0 &&
      (iteration % config_.mccfr_prune_full_traversal_interval) == 0) {
    return mask;
  }

  std::fill(mask.begin(), mask.end(), false);
  std::size_t best_index = 0;
  for (std::size_t i = 1; i < action_count; ++i) {
    if (node.regret_sum[i] > node.regret_sum[best_index]) {
      best_index = i;
    }
  }
  mask[best_index] = true;
  for (std::size_t i = 0; i < action_count; ++i) {
    if (node.regret_sum[i] > config_.mccfr_prune_regret_threshold) {
      mask[i] = true;
    }
  }

  std::size_t selected = 0;
  for (bool keep : mask) {
    selected += keep ? 1 : 0;
  }

  if (selected >= min_actions) {
    return mask;
  }

  std::vector<std::size_t> order(action_count, 0);
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
    return node.regret_sum[left] > node.regret_sum[right];
  });
  for (std::size_t index : order) {
    if (!mask[index]) {
      mask[index] = true;
      selected += 1;
      if (selected >= min_actions) {
        break;
      }
    }
  }
  return mask;
}

double MccfrTrainer::Traverse(const game::GameState& state,
                              PlayerId update_player,
                              std::uint64_t iteration,
                              double reach_opponent_target,
                              double reach_sampling) {
  if (state.IsTerminal()) {
    return state.TerminalUtility(update_player);
  }

  if (state.IsChanceNode()) {
    const auto outcomes = state.ChanceOutcomes();
    if (outcomes.empty()) {
      throw std::runtime_error("Chance node has no outcomes.");
    }
    std::vector<double> probs;
    probs.reserve(outcomes.size());
    for (const auto& outcome : outcomes) {
      probs.push_back(outcome.probability);
    }
    const std::size_t sampled = SampleIndex(probs);
    const auto next = state.CloneAndApplyChance(outcomes[sampled].id);
    const double probability = probs[sampled];
    return Traverse(*next,
                    update_player,
                    iteration,
                    reach_opponent_target * probability,
                    reach_sampling * probability);
  }

  const auto actions = state.LegalActions();
  if (actions.empty()) {
    throw std::runtime_error("Non-terminal decision node has no legal actions.");
  }

  const PlayerId current_player = state.CurrentPlayer();
  const std::string key = state.InfosetKey(current_player);
  InfosetNode& node = table_.GetOrCreate(key, actions.size());
  const auto strategy = RegretMatcher::ComputeStrategyPlus(node.regret_sum);

  // Correct sampled trajectories back to target-opponent/chance reach.
  const double importance_weight =
      reach_sampling > 0.0 ? (reach_opponent_target / reach_sampling) : 0.0;
  const double average_weight = iteration > config_.averaging_delay
                                    ? static_cast<double>(iteration - config_.averaging_delay)
                                    : 1.0;

  if (current_player == update_player) {
    for (std::size_t i = 0; i < strategy.size(); ++i) {
      node.strategy_sum[i] += average_weight * importance_weight * strategy[i];
    }

    std::vector<double> action_utilities(actions.size(), 0.0);
    const auto active_mask = BuildPrunedActionMask(node, actions.size(), iteration);
    double active_mass = 0.0;
    double active_weighted_utility = 0.0;
    for (std::size_t i = 0; i < actions.size(); ++i) {
      if (!active_mask[i]) {
        continue;
      }
      const auto next = state.CloneAndApplyAction(actions[i]);
      action_utilities[i] =
          Traverse(*next, update_player, iteration, reach_opponent_target, reach_sampling);
      active_mass += strategy[i];
      active_weighted_utility += strategy[i] * action_utilities[i];
    }

    double fallback_utility = 0.0;
    if (active_mass > 0.0) {
      fallback_utility = active_weighted_utility / active_mass;
    }
    for (std::size_t i = 0; i < actions.size(); ++i) {
      if (!active_mask[i]) {
        action_utilities[i] = fallback_utility;
      }
    }

    double node_utility = 0.0;
    for (std::size_t i = 0; i < actions.size(); ++i) {
      node_utility += strategy[i] * action_utilities[i];
    }

    for (std::size_t i = 0; i < actions.size(); ++i) {
      if (!active_mask[i]) {
        continue;
      }
      const double regret = importance_weight * (action_utilities[i] - node_utility);
      node.regret_sum[i] = std::max(0.0, node.regret_sum[i] + regret);
    }
    return node_utility;
  }

  const auto sampling_strategy = BuildSamplingDistribution(strategy);
  const std::size_t sampled_index = SampleIndex(sampling_strategy);
  const auto next = state.CloneAndApplyAction(actions[sampled_index]);
  return Traverse(*next,
                  update_player,
                  iteration,
                  reach_opponent_target * strategy[sampled_index],
                  reach_sampling * sampling_strategy[sampled_index]);
}

std::size_t MccfrTrainer::SampleIndex(const std::vector<double>& probs) {
  if (probs.empty()) {
    throw std::runtime_error("Cannot sample from empty probability vector.");
  }
  std::discrete_distribution<std::size_t> distribution(probs.begin(), probs.end());
  return distribution(rng_);
}

std::vector<double> MccfrTrainer::BuildSamplingDistribution(
    const std::vector<double>& strategy) const {
  if (strategy.empty()) {
    return {};
  }

  if (config_.sampling_epsilon <= 0.0) {
    return strategy;
  }

  const double epsilon = std::clamp(config_.sampling_epsilon, 0.0, 1.0);
  const double uniform = 1.0 / static_cast<double>(strategy.size());
  std::vector<double> sampling(strategy.size(), 0.0);
  for (std::size_t i = 0; i < strategy.size(); ++i) {
    sampling[i] = (1.0 - epsilon) * strategy[i] + epsilon * uniform;
  }
  return sampling;
}

}  // namespace plarbius::cfr
