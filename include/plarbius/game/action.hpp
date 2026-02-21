#pragma once

#include <string_view>

namespace plarbius::game {

enum class ActionType {
  kCheck,
  kBet,
  kCall,
  kFold
};

struct Action {
  ActionType type{};
};

std::string_view ToString(ActionType type);

}  // namespace plarbius::game

