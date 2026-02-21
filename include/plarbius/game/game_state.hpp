#pragma once

#include <memory>
#include <string>
#include <vector>

#include "plarbius/core/types.hpp"
#include "plarbius/game/action.hpp"
#include "plarbius/game/chance_outcome.hpp"

namespace plarbius::game {

class GameState {
 public:
  virtual ~GameState() = default;

  virtual bool IsTerminal() const = 0;
  virtual bool IsChanceNode() const = 0;
  virtual PlayerId CurrentPlayer() const = 0;
  virtual double TerminalUtility(PlayerId player) const = 0;

  virtual std::vector<Action> LegalActions() const = 0;
  virtual std::unique_ptr<GameState> CloneAndApplyAction(Action action) const = 0;

  virtual std::vector<ChanceOutcome> ChanceOutcomes() const = 0;
  virtual std::unique_ptr<GameState> CloneAndApplyChance(int outcome_id) const = 0;

  virtual std::string InfosetKey(PlayerId player) const = 0;
};

}  // namespace plarbius::game

