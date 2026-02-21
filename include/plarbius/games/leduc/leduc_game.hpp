#pragma once

#include "plarbius/game/game.hpp"

namespace plarbius::games::leduc {

class LeducGame final : public game::Game {
 public:
  std::string Name() const override;
  std::unique_ptr<game::GameState> NewInitialState() const override;
};

}  // namespace plarbius::games::leduc

