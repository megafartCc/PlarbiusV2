#include "plarbius/game/action.hpp"

namespace plarbius::game {

std::string_view ToString(ActionType type) {
  switch (type) {
    case ActionType::kCheck:
      return "check";
    case ActionType::kBet:
      return "bet";
    case ActionType::kCall:
      return "call";
    case ActionType::kFold:
      return "fold";
  }
  return "unknown";
}

}  // namespace plarbius::game

