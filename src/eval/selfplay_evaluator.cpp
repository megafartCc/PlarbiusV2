#include "plarbius/eval/selfplay_evaluator.hpp"

#include <future>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "plarbius/core/types.hpp"

namespace plarbius::eval {

namespace {

struct EvalAccumulator {
  double utility_p0 = 0.0;
  std::size_t evaluated_states = 0;
};

double EvaluateRecursive(const game::GameState& state,
                         const policy::PolicyTable& policy_p0,
                         const policy::PolicyTable& policy_p1,
                         std::size_t* state_counter) {
  ++(*state_counter);
  if (state.IsTerminal()) {
    return state.TerminalUtility(kPlayer0);
  }

  if (state.IsChanceNode()) {
    double value = 0.0;
    for (const auto& outcome : state.ChanceOutcomes()) {
      const auto next = state.CloneAndApplyChance(outcome.id);
      value += outcome.probability * EvaluateRecursive(*next, policy_p0, policy_p1, state_counter);
    }
    return value;
  }

  const auto actions = state.LegalActions();
  if (actions.empty()) {
    throw std::runtime_error("Decision node has no legal actions.");
  }

  const PlayerId current_player = state.CurrentPlayer();
  const auto probs = policy::GetActionDistribution(
      current_player == kPlayer0 ? policy_p0 : policy_p1,
      state.InfosetKey(current_player),
      actions.size());

  double value = 0.0;
  for (std::size_t i = 0; i < actions.size(); ++i) {
    const auto next = state.CloneAndApplyAction(actions[i]);
    value += probs[i] * EvaluateRecursive(*next, policy_p0, policy_p1, state_counter);
  }
  return value;
}

EvalAccumulator EvaluateSingleThread(const game::GameState& root,
                                     const policy::PolicyTable& policy_p0,
                                     const policy::PolicyTable& policy_p1) {
  EvalAccumulator out;
  out.utility_p0 = EvaluateRecursive(root, policy_p0, policy_p1, &out.evaluated_states);
  return out;
}

}  // namespace

SelfplayReport EvaluateExpectedSelfplay(const game::Game& game,
                                        const policy::PolicyTable& policy_p0,
                                        const policy::PolicyTable& policy_p1,
                                        std::size_t num_threads) {
  auto root = game.NewInitialState();

  EvalAccumulator total;
  if (num_threads <= 1 || !root->IsChanceNode()) {
    total = EvaluateSingleThread(*root, policy_p0, policy_p1);
  } else {
    const auto outcomes = root->ChanceOutcomes();
    if (outcomes.empty()) {
      throw std::runtime_error("Chance root has no outcomes.");
    }

    const std::size_t workers = std::min<std::size_t>(num_threads, outcomes.size());
    std::vector<std::future<EvalAccumulator>> tasks;
    tasks.reserve(workers);

    for (std::size_t worker = 0; worker < workers; ++worker) {
      tasks.push_back(std::async(std::launch::async, [&, worker]() -> EvalAccumulator {
        EvalAccumulator partial;
        for (std::size_t i = worker; i < outcomes.size(); i += workers) {
          const auto next = root->CloneAndApplyChance(outcomes[i].id);
          std::size_t states = 0;
          const double value = EvaluateRecursive(*next, policy_p0, policy_p1, &states);
          partial.utility_p0 += outcomes[i].probability * value;
          partial.evaluated_states += states;
        }
        return partial;
      }));
    }

    for (auto& task : tasks) {
      const auto partial = task.get();
      total.utility_p0 += partial.utility_p0;
      total.evaluated_states += partial.evaluated_states;
    }
  }

  SelfplayReport report;
  report.utility_p0 = total.utility_p0;
  report.utility_p1 = -total.utility_p0;
  report.evaluated_states = total.evaluated_states;
  return report;
}

}  // namespace plarbius::eval

