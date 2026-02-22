#pragma once

#include <cstdint>
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
  std::uint16_t abstraction_id = 0;
  double amount = 0.0;
};

std::string_view ToString(ActionType type);

}  // namespace plarbius::game
