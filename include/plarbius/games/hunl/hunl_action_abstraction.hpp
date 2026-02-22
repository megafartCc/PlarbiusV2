#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "plarbius/game/action.hpp"

namespace plarbius::games::hunl {

struct HunlActionAbstractionConfig {
  std::vector<double> preflop_raise_sizes_bb = {2.0, 6.0, 14.0};
  std::vector<double> postflop_bet_sizes_pot = {0.33, 0.75, 1.25};
  std::vector<double> postflop_raise_sizes_pot = {0.75, 1.5};
  std::size_t max_raises_per_round = 2;
  bool allow_all_in = true;
  double min_bet_bb = 1.0;
};

HunlActionAbstractionConfig DefaultHunlActionAbstractionConfig();
HunlActionAbstractionConfig LoadHunlActionAbstractionConfig(const std::string& path);
void SaveHunlActionAbstractionConfig(const HunlActionAbstractionConfig& config,
                                     const std::string& path);

struct HunlLegalActionContext {
  std::size_t street = 0;
  bool facing_bet = false;
  double pot_bb = 0.0;
  double to_call_bb = 0.0;
  double stack_to_act_bb = 0.0;
  std::size_t raises_in_round = 0;
};

std::vector<game::Action> BuildHunlLegalActions(const HunlActionAbstractionConfig& config,
                                                const HunlLegalActionContext& context);

std::string DescribeHunlAction(const game::Action& action);

}  // namespace plarbius::games::hunl
