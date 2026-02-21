#include "plarbius/games/leduc/leduc_game.hpp"

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "plarbius/core/types.hpp"
#include "plarbius/game/action.hpp"
#include "plarbius/game/chance_outcome.hpp"
#include "plarbius/game/game_state.hpp"

namespace plarbius::games::leduc {

namespace {

std::vector<std::array<int, 3>> BuildDeals() {
  std::vector<std::array<int, 3>> deals;
  deals.reserve(120);
  for (int private_0 = 0; private_0 < 6; ++private_0) {
    for (int private_1 = 0; private_1 < 6; ++private_1) {
      if (private_1 == private_0) {
        continue;
      }
      for (int public_card = 0; public_card < 6; ++public_card) {
        if (public_card == private_0 || public_card == private_1) {
          continue;
        }
        deals.push_back({private_0, private_1, public_card});
      }
    }
  }
  return deals;
}

const std::vector<std::array<int, 3>>& Deals() {
  static const std::vector<std::array<int, 3>> deals = BuildDeals();
  return deals;
}

char RankToChar(int rank) {
  switch (rank) {
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

int CardRank(int card) {
  if (card < 0 || card >= 6) {
    throw std::out_of_range("Card index out of range.");
  }
  return card / 2;
}

class LeducState final : public game::GameState {
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

    const int pot = contributions_[0] + contributions_[1];
    if (winner_ == -1) {
      return static_cast<double>(pot) / 2.0 - static_cast<double>(contributions_[player]);
    }
    if (winner_ == static_cast<int>(player)) {
      return static_cast<double>(pot - contributions_[player]);
    }
    return -static_cast<double>(contributions_[player]);
  }

  std::vector<game::Action> LegalActions() const override {
    if (!dealt_ || terminal_) {
      return {};
    }

    if (round_history_.empty() || round_history_ == "c") {
      return {
          game::Action{game::ActionType::kCheck},
          game::Action{game::ActionType::kBet},
      };
    }
    if (round_history_ == "b" || round_history_ == "cb") {
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
    auto next = std::make_unique<LeducState>(*this);
    next->ApplyAction(action);
    return next;
  }

  std::vector<game::ChanceOutcome> ChanceOutcomes() const override {
    if (!IsChanceNode()) {
      return {};
    }

    std::vector<game::ChanceOutcome> outcomes;
    outcomes.reserve(Deals().size());
    const double p = 1.0 / static_cast<double>(Deals().size());
    for (std::size_t i = 0; i < Deals().size(); ++i) {
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
    if (outcome_id < 0 || outcome_id >= static_cast<int>(Deals().size())) {
      throw std::out_of_range("Chance outcome id out of range.");
    }

    auto next = std::make_unique<LeducState>(*this);
    next->dealt_ = true;
    next->cards_[0] = Deals()[static_cast<std::size_t>(outcome_id)][0];
    next->cards_[1] = Deals()[static_cast<std::size_t>(outcome_id)][1];
    next->public_card_ = Deals()[static_cast<std::size_t>(outcome_id)][2];
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
    key.reserve(32);
    key.push_back(static_cast<char>('0' + player));
    key.push_back('|');
    key.push_back(RankToChar(CardRank(cards_[player])));
    key.push_back('|');
    if (round_ == 0) {
      key.push_back('_');
    } else {
      key.push_back(RankToChar(CardRank(public_card_)));
    }
    key.push_back('|');
    key.push_back('r');
    key.push_back(static_cast<char>('0' + round_));
    key.push_back('|');
    if (history_.empty() && round_history_.empty()) {
      key.push_back('_');
    } else if (history_.empty()) {
      key.append(round_history_);
    } else if (round_history_.empty()) {
      key.append(history_);
      key.append("/_");
    } else {
      key.append(history_);
      key.push_back('/');
      key.append(round_history_);
    }
    return key;
  }

 private:
  [[nodiscard]] int BetSize() const {
    return round_ == 0 ? 2 : 4;
  }

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
    if (round_history_.empty()) {
      round_history_ = "c";
      current_player_ = 1 - current_player_;
      return;
    }
    if (round_history_ == "c") {
      round_history_ = "cc";
      EndRound();
      return;
    }
    throw std::runtime_error("Invalid check transition.");
  }

  void ApplyBet() {
    if (round_history_.empty()) {
      round_history_ = "b";
      contributions_[current_player_] += BetSize();
      current_player_ = 1 - current_player_;
      return;
    }
    if (round_history_ == "c") {
      round_history_ = "cb";
      contributions_[current_player_] += BetSize();
      current_player_ = 1 - current_player_;
      return;
    }
    throw std::runtime_error("Invalid bet transition.");
  }

  void ApplyCall() {
    if (round_history_ == "b" || round_history_ == "cb") {
      round_history_.push_back('c');
      contributions_[current_player_] += BetSize();
      EndRound();
      return;
    }
    throw std::runtime_error("Invalid call transition.");
  }

  void ApplyFold() {
    if (round_history_ == "b" || round_history_ == "cb") {
      round_history_.push_back('f');
      terminal_ = true;
      winner_ = 1 - current_player_;
      return;
    }
    throw std::runtime_error("Invalid fold transition.");
  }

  void EndRound() {
    if (!history_.empty()) {
      history_.push_back('/');
    }
    history_.append(round_history_);
    round_history_.clear();

    if (round_ == 0) {
      round_ = 1;
      current_player_ = kPlayer0;
      return;
    }

    terminal_ = true;
    winner_ = ShowdownWinner();
  }

  [[nodiscard]] int ShowdownWinner() const {
    const int rank_0 = CardRank(cards_[0]);
    const int rank_1 = CardRank(cards_[1]);
    const int board_rank = CardRank(public_card_);

    const bool pair_0 = rank_0 == board_rank;
    const bool pair_1 = rank_1 == board_rank;
    if (pair_0 != pair_1) {
      return pair_0 ? 0 : 1;
    }
    if (rank_0 > rank_1) {
      return 0;
    }
    if (rank_1 > rank_0) {
      return 1;
    }
    return -1;
  }

  bool dealt_ = false;
  bool terminal_ = false;
  PlayerId current_player_ = kPlayer0;
  int round_ = 0;
  std::array<int, 2> cards_ = {-1, -1};
  int public_card_ = -1;
  std::array<int, 2> contributions_ = {1, 1};
  std::string history_;
  std::string round_history_;
  int winner_ = -2;
};

}  // namespace

std::string LeducGame::Name() const {
  return "LeducPoker";
}

std::unique_ptr<game::GameState> LeducGame::NewInitialState() const {
  return std::make_unique<LeducState>();
}

}  // namespace plarbius::games::leduc

