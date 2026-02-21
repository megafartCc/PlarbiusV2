#pragma once

#include <memory>
#include <string>

#include "plarbius/game/game_state.hpp"

namespace plarbius::game {

class Game {
 public:
  virtual ~Game() = default;

  virtual std::string Name() const = 0;
  virtual std::unique_ptr<GameState> NewInitialState() const = 0;
};

}  // namespace plarbius::game

