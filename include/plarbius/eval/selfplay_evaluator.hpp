#pragma once

#include <cstddef>

#include "plarbius/game/game.hpp"
#include "plarbius/policy/policy_io.hpp"

namespace plarbius::eval {

struct SelfplayReport {
  double utility_p0 = 0.0;
  double utility_p1 = 0.0;
  std::size_t evaluated_states = 0;
};

SelfplayReport EvaluateExpectedSelfplay(const game::Game& game,
                                        const policy::PolicyTable& policy_p0,
                                        const policy::PolicyTable& policy_p1,
                                        std::size_t num_threads = 1);

}  // namespace plarbius::eval

