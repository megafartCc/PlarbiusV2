#include "plarbius/games/kuhn/kuhn_game.hpp"

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "plarbius/game/action.hpp"
#include "plarbius/game/chance_outcome.hpp"
#include "plarbius/game/game_state.hpp"

namespace plarbius::games::kuhn {

namespace {

constexpr std::array<std::array<int, 2>, 6> kDeals = {
    std::array<int, 2>{0, 1},
    std::array<int, 2>{0, 2},
    std::array<int, 2>{1, 0},
    std::array<int, 2>{1, 2},
    std::array<int, 2>{2, 0},
    std::array<int, 2>{2, 1},
};

char CardToChar(int card) {
  switch (card) {
    case 0:
      return 'J';
    case 1:
      return 'Q';
    case 2:
      return 'K';
    default:
      return '?';
  }
}

class KuhnState final : public game::GameState {
 public:
  bool IsTerminal() const override {
    return terminal_;
  }

  bool IsChanceNode() const override {
    return !dealt_ && !terminal_;
  }

  PlayerId CurrentPlayer() const override {
    return current_player_;
  }

  double TerminalUtility(PlayerId player) const override {
    if (!terminal_) {
      throw std::runtime_error("Terminal utility requested on non-terminal state.");
    }
    const int winner = winner_;
    if (winner == static_cast<int>(player)) {
      return static_cast<double>(contributions_[1 - player]);
    }
    return -static_cast<double>(contributions_[player]);
  }

  std::vector<game::Action> LegalActions() const override {
    if (terminal_ || !dealt_) {
      return {};
    }

    if (history_.empty() || history_ == "c") {
      return {
          game::Action{game::ActionType::kCheck},
          game::Action{game::ActionType::kBet},
      };
    }

    if (history_ == "b" || history_ == "cb") {
      return {
          game::Action{game::ActionType::kCall},
          game::Action{game::ActionType::kFold},
      };
    }

    return {};
  }

  std::unique_ptr<game::GameState> CloneAndApplyAction(game::Action action) const override {
    if (!dealt_ || terminal_) {
      throw std::runtime_error("Cannot apply action to this state.");
    }
    auto next = std::make_unique<KuhnState>(*this);
    next->ApplyAction(action);
    return next;
  }

  std::vector<game::ChanceOutcome> ChanceOutcomes() const override {
    if (!IsChanceNode()) {
      return {};
    }

    std::vector<game::ChanceOutcome> outcomes;
    outcomes.reserve(kDeals.size());
    const double p = 1.0 / static_cast<double>(kDeals.size());
    for (std::size_t i = 0; i < kDeals.size(); ++i) {
      outcomes.push_back(game::ChanceOutcome{
          static_cast<int>(i),
          p,
      });
    }
    return outcomes;
  }

  std::unique_ptr<game::GameState> CloneAndApplyChance(int outcome_id) const override {
    if (!IsChanceNode()) {
      throw std::runtime_error("Chance action applied on non-chance node.");
    }
    if (outcome_id < 0 || outcome_id >= static_cast<int>(kDeals.size())) {
      throw std::out_of_range("Chance outcome id out of range.");
    }

    auto next = std::make_unique<KuhnState>(*this);
    next->dealt_ = true;
    next->cards_[0] = kDeals[static_cast<std::size_t>(outcome_id)][0];
    next->cards_[1] = kDeals[static_cast<std::size_t>(outcome_id)][1];
    next->current_player_ = kPlayer0;
    return next;
  }

  std::string InfosetKey(PlayerId player) const override {
    if (!dealt_ || terminal_) {
      return {};
    }
    if (player > 1) {
      throw std::out_of_range("Invalid player id.");
    }

    std::string key;
    key.reserve(16);
    key.push_back(static_cast<char>('0' + player));
    key.push_back('|');
    key.push_back(CardToChar(cards_[player]));
    key.push_back('|');
    if (history_.empty()) {
      key.push_back('_');
    } else {
      key.append(history_);
    }
    return key;
  }

 private:
  void ApplyAction(game::Action action) {
    switch (action.type) {
      case game::ActionType::kCheck:
        ApplyCheck();
        return;
      case game::ActionType::kBet:
        ApplyBet();
        return;
      case game::ActionType::kCall:
        ApplyCall();
        return;
      case game::ActionType::kFold:
        ApplyFold();
        return;
    }
    throw std::runtime_error("Unknown action type.");
  }

  void ApplyCheck() {
    if (history_.empty()) {
      history_ = "c";
      current_player_ = kPlayer1;
      return;
    }
    if (history_ == "c") {
      history_ = "cc";
      terminal_ = true;
      winner_ = ShowdownWinner();
      return;
    }
    throw std::runtime_error("Invalid check transition.");
  }

  void ApplyBet() {
    if (history_.empty()) {
      history_ = "b";
      contributions_[0] += 1;
      current_player_ = kPlayer1;
      return;
    }
    if (history_ == "c") {
      history_ = "cb";
      contributions_[1] += 1;
      current_player_ = kPlayer0;
      return;
    }
    throw std::runtime_error("Invalid bet transition.");
  }

  void ApplyCall() {
    if (history_ == "b") {
      history_ = "bc";
      contributions_[1] += 1;
      terminal_ = true;
      winner_ = ShowdownWinner();
      return;
    }
    if (history_ == "cb") {
      history_ = "cbc";
      contributions_[0] += 1;
      terminal_ = true;
      winner_ = ShowdownWinner();
      return;
    }
    throw std::runtime_error("Invalid call transition.");
  }

  void ApplyFold() {
    if (history_ == "b") {
      history_ = "bf";
      terminal_ = true;
      winner_ = 0;
      return;
    }
    if (history_ == "cb") {
      history_ = "cbf";
      terminal_ = true;
      winner_ = 1;
      return;
    }
    throw std::runtime_error("Invalid fold transition.");
  }

  int ShowdownWinner() const {
    if (!dealt_) {
      throw std::runtime_error("Cards are not dealt.");
    }
    return cards_[0] > cards_[1] ? 0 : 1;
  }

  bool dealt_ = false;
  bool terminal_ = false;
  PlayerId current_player_ = kPlayer0;
  std::array<int, 2> cards_ = {-1, -1};
  std::array<int, 2> contributions_ = {1, 1};
  std::string history_;
  int winner_ = -1;
};

}  // namespace

std::string KuhnGame::Name() const {
  return "KuhnPoker";
}

std::unique_ptr<game::GameState> KuhnGame::NewInitialState() const {
  return std::make_unique<KuhnState>();
}

}  // namespace plarbius::games::kuhn

