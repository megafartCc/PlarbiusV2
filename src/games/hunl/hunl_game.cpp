#include "plarbius/games/hunl/hunl_game.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "plarbius/core/types.hpp"
#include "plarbius/game/chance_outcome.hpp"
#include "plarbius/game/game_state.hpp"
#include "plarbius/games/hunl/hunl_action_abstraction.hpp"

namespace plarbius::games::hunl {

namespace {

constexpr std::size_t kStreetCount = 4;
constexpr double kEpsilon = 1e-9;
constexpr std::array<double, kStreetCount> kStreetStrengthWeight = {0.2, 0.35, 0.55, 1.0};

std::uint64_t Mix(std::uint64_t x) {
  x ^= x >> 33U;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33U;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33U;
  return x;
}

std::uint32_t HashBucket(std::uint32_t outcome_id, std::size_t street, std::size_t slot, std::uint32_t mod) {
  if (mod == 0) {
    return 0;
  }
  std::uint64_t seed = static_cast<std::uint64_t>(outcome_id + 1U);
  seed ^= static_cast<std::uint64_t>((street + 1U) * 0x9e37U);
  seed ^= static_cast<std::uint64_t>((slot + 1U) * 0x85ebU);
  return static_cast<std::uint32_t>(Mix(seed) % static_cast<std::uint64_t>(mod));
}

std::uint32_t PrivateBucketCount(const HunlBucketConfig& config, std::size_t street) {
  switch (street) {
    case 0:
      return config.preflop_private_buckets;
    case 1:
      return config.flop_private_buckets;
    case 2:
      return config.turn_private_buckets;
    case 3:
      return config.river_private_buckets;
    default:
      throw std::out_of_range("Invalid street for private bucket count.");
  }
}

std::uint32_t PublicBucketCount(const HunlBucketConfig& config, std::size_t street) {
  switch (street) {
    case 0:
      return 1;
    case 1:
      return config.flop_public_buckets;
    case 2:
      return config.turn_public_buckets;
    case 3:
      return config.river_public_buckets;
    default:
      throw std::out_of_range("Invalid street for public bucket count.");
  }
}

PlayerId FirstPlayerForStreet(std::size_t street) {
  return street == 0 ? kPlayer0 : kPlayer1;
}

std::uint32_t CoarseBucket(double value, double width, std::uint32_t max_bucket) {
  if (value <= 0.0) {
    return 0;
  }
  const std::uint32_t bucket = static_cast<std::uint32_t>(value / width);
  return bucket > max_bucket ? max_bucket : bucket;
}

class HunlState final : public game::GameState {
 public:
  explicit HunlState(const HunlGameConfig* config) : config_(config) {
    if (config_ == nullptr) {
      throw std::invalid_argument("HunlState requires a valid config.");
    }
    stack_remaining_ = {config_->stack_bb, config_->stack_bb};
    PostBlind(kPlayer0, config_->small_blind_bb);
    PostBlind(kPlayer1, config_->big_blind_bb);
    current_bet_bb_ = committed_round_bb_[kPlayer1];
    current_player_ = FirstPlayerForStreet(street_);
  }

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
      throw std::runtime_error("Terminal utility requested on non-terminal HUNL state.");
    }
    const double pot = committed_total_bb_[0] + committed_total_bb_[1];
    if (winner_ == -1) {
      return (pot / 2.0) - committed_total_bb_[player];
    }
    if (winner_ == static_cast<int>(player)) {
      return pot - committed_total_bb_[player];
    }
    return -committed_total_bb_[player];
  }

  std::vector<game::Action> LegalActions() const override {
    if (!dealt_ || terminal_) {
      return {};
    }
    HunlLegalActionContext context;
    context.street = street_;
    context.facing_bet = ToCallFor(current_player_) > kEpsilon;
    context.pot_bb = committed_total_bb_[0] + committed_total_bb_[1];
    context.to_call_bb = ToCallFor(current_player_);
    context.stack_to_act_bb = stack_remaining_[current_player_];
    context.raises_in_round = raises_in_round_;
    return BuildHunlLegalActions(config_->action_config, context);
  }

  std::unique_ptr<game::GameState> CloneAndApplyAction(game::Action action) const override {
    if (!dealt_ || terminal_) {
      throw std::runtime_error("Cannot apply action on this HUNL state.");
    }
    auto next = std::make_unique<HunlState>(*this);
    next->ApplyAction(action);
    return next;
  }

  std::vector<game::ChanceOutcome> ChanceOutcomes() const override {
    if (!IsChanceNode()) {
      return {};
    }
    const std::uint32_t outcomes = config_->bucket_config.chance_outcomes;
    std::vector<game::ChanceOutcome> out;
    out.reserve(outcomes);
    const double probability = 1.0 / static_cast<double>(outcomes);
    for (std::uint32_t i = 0; i < outcomes; ++i) {
      out.push_back(game::ChanceOutcome{static_cast<int>(i), probability});
    }
    return out;
  }

  std::unique_ptr<game::GameState> CloneAndApplyChance(int outcome_id) const override {
    if (!IsChanceNode()) {
      throw std::runtime_error("Chance action applied to non-chance HUNL state.");
    }
    if (outcome_id < 0 ||
        outcome_id >= static_cast<int>(config_->bucket_config.chance_outcomes)) {
      throw std::out_of_range("HUNL chance outcome id out of range.");
    }
    auto next = std::make_unique<HunlState>(*this);
    next->dealt_ = true;
    next->AssignAbstractDeal(static_cast<std::uint32_t>(outcome_id));
    return next;
  }

  std::string InfosetKey(PlayerId player) const override {
    if (!dealt_ || terminal_) {
      return {};
    }
    if (player > 1) {
      throw std::out_of_range("Invalid player id for HUNL infoset.");
    }

    const double pot_bb = committed_total_bb_[0] + committed_total_bb_[1];
    const double to_call_bb = ToCallFor(player);
    const double stack_bb = stack_remaining_[player];

    std::ostringstream key;
    key << static_cast<int>(player)
        << "|s" << street_
        << "|pr" << private_bucket_[street_][player]
        << "|pb" << public_bucket_[street_]
        << "|pot" << CoarseBucket(pot_bb, 5.0, 100)
        << "|tc" << CoarseBucket(to_call_bb, 2.0, 50)
        << "|stk" << CoarseBucket(stack_bb, 5.0, 100)
        << "|r" << raises_in_round_
        << '|';
    if (history_.empty() && round_history_.empty()) {
      key << '_';
    } else if (history_.empty()) {
      key << round_history_;
    } else if (round_history_.empty()) {
      key << history_ << "/_";
    } else {
      key << history_ << '/' << round_history_;
    }
    return key.str();
  }

 private:
  void PostBlind(PlayerId player, double amount_bb) {
    const double pay = std::min(amount_bb, stack_remaining_[player]);
    stack_remaining_[player] -= pay;
    committed_total_bb_[player] += pay;
    committed_round_bb_[player] += pay;
  }

  double ToCallFor(PlayerId player) const {
    return std::max(0.0, current_bet_bb_ - committed_round_bb_[player]);
  }

  void ApplyPayment(PlayerId player, double amount_bb) {
    const double pay = std::min(std::max(0.0, amount_bb), stack_remaining_[player]);
    stack_remaining_[player] -= pay;
    committed_round_bb_[player] += pay;
    committed_total_bb_[player] += pay;
  }

  void AppendRoundToken(const std::string& token) {
    if (!round_history_.empty()) {
      round_history_.push_back('.');
    }
    round_history_.append(token);
  }

  void RecordRoundHistory() {
    if (!history_.empty()) {
      history_.push_back('/');
    }
    if (round_history_.empty()) {
      history_.push_back('_');
    } else {
      history_.append(round_history_);
    }
    round_history_.clear();
  }

  void ResolveShowdown() {
    const double score_p0 = ShowdownScore(kPlayer0);
    const double score_p1 = ShowdownScore(kPlayer1);
    terminal_ = true;
    if (std::fabs(score_p0 - score_p1) < 1e-9) {
      winner_ = -1;
      return;
    }
    winner_ = score_p0 > score_p1 ? 0 : 1;
  }

  void EndRound() {
    RecordRoundHistory();
    if (stack_remaining_[0] <= kEpsilon || stack_remaining_[1] <= kEpsilon) {
      ResolveShowdown();
      return;
    }
    if (street_ + 1 >= kStreetCount) {
      ResolveShowdown();
      return;
    }

    street_ += 1;
    current_player_ = FirstPlayerForStreet(street_);
    committed_round_bb_ = {0.0, 0.0};
    current_bet_bb_ = 0.0;
    raises_in_round_ = 0;
    actions_in_round_ = 0;
  }

  void ApplyCheck() {
    if (ToCallFor(current_player_) > kEpsilon) {
      throw std::runtime_error("Check is invalid when facing a bet.");
    }
    AppendRoundToken("x");
    actions_in_round_ += 1;
    if (actions_in_round_ >= 2) {
      EndRound();
    } else {
      current_player_ = 1 - current_player_;
    }
  }

  void ApplyCall() {
    const double to_call = ToCallFor(current_player_);
    if (to_call <= kEpsilon) {
      throw std::runtime_error("Call is invalid when there is no outstanding bet.");
    }
    ApplyPayment(current_player_, to_call);
    AppendRoundToken("c");
    actions_in_round_ += 1;
    EndRound();
  }

  void ApplyFold() {
    if (ToCallFor(current_player_) <= kEpsilon) {
      throw std::runtime_error("Fold is invalid when there is no outstanding bet.");
    }
    AppendRoundToken("f");
    terminal_ = true;
    winner_ = 1 - current_player_;
  }

  void ApplyBetOrRaise(const game::Action& action) {
    double to_call = ToCallFor(current_player_);
    double pay = std::max(0.0, action.amount);
    if (pay <= kEpsilon) {
      throw std::runtime_error("Bet action must carry a positive amount.");
    }
    if (to_call > kEpsilon && pay <= to_call + kEpsilon) {
      throw std::runtime_error("Raise amount must exceed call amount.");
    }
    pay = std::min(pay, stack_remaining_[current_player_]);
    ApplyPayment(current_player_, pay);
    current_bet_bb_ = std::max(current_bet_bb_, committed_round_bb_[current_player_]);
    raises_in_round_ += 1;
    actions_in_round_ += 1;
    AppendRoundToken(DescribeHunlAction(action));

    if (stack_remaining_[0] <= kEpsilon && stack_remaining_[1] <= kEpsilon) {
      ResolveShowdown();
      return;
    }
    current_player_ = 1 - current_player_;
    if (ToCallFor(current_player_) <= kEpsilon &&
        (stack_remaining_[0] <= kEpsilon || stack_remaining_[1] <= kEpsilon)) {
      ResolveShowdown();
    }
  }

  void ApplyAction(const game::Action& action) {
    switch (action.type) {
      case game::ActionType::kCheck:
        ApplyCheck();
        return;
      case game::ActionType::kCall:
        ApplyCall();
        return;
      case game::ActionType::kFold:
        ApplyFold();
        return;
      case game::ActionType::kBet:
        ApplyBetOrRaise(action);
        return;
    }
    throw std::runtime_error("Unknown action in HUNL state.");
  }

  void AssignAbstractDeal(std::uint32_t outcome_id) {
    for (std::size_t street = 0; street < kStreetCount; ++street) {
      const std::uint32_t private_count = PrivateBucketCount(config_->bucket_config, street);
      const std::uint32_t public_count = PublicBucketCount(config_->bucket_config, street);
      for (std::size_t player = 0; player < 2; ++player) {
        private_bucket_[street][player] = HashBucket(outcome_id, street, player, private_count);
      }
      public_bucket_[street] = HashBucket(outcome_id, street, 2, public_count);
    }
  }

  double NormalizedBucket(std::uint32_t bucket, std::uint32_t count) const {
    if (count <= 1) {
      return 0.0;
    }
    return static_cast<double>(bucket) / static_cast<double>(count - 1);
  }

  double ShowdownScore(PlayerId player) const {
    double score = 0.0;
    for (std::size_t street = 0; street < kStreetCount; ++street) {
      const double private_part = NormalizedBucket(
          private_bucket_[street][player],
          PrivateBucketCount(config_->bucket_config, street));
      const double public_part = NormalizedBucket(
          public_bucket_[street],
          PublicBucketCount(config_->bucket_config, street));
      score += kStreetStrengthWeight[street] * (0.7 * private_part + 0.3 * public_part);
    }
    return score;
  }

  const HunlGameConfig* config_ = nullptr;
  bool dealt_ = false;
  bool terminal_ = false;
  PlayerId current_player_ = kPlayer0;
  std::size_t street_ = 0;
  std::array<double, 2> stack_remaining_ = {0.0, 0.0};
  std::array<double, 2> committed_total_bb_ = {0.0, 0.0};
  std::array<double, 2> committed_round_bb_ = {0.0, 0.0};
  double current_bet_bb_ = 0.0;
  std::size_t raises_in_round_ = 0;
  std::size_t actions_in_round_ = 0;
  std::array<std::array<std::uint32_t, 2>, kStreetCount> private_bucket_ = {};
  std::array<std::uint32_t, kStreetCount> public_bucket_ = {};
  std::string history_;
  std::string round_history_;
  int winner_ = -2;
};

}  // namespace

HunlGameConfig DefaultHunlGameConfig() {
  return HunlGameConfig{};
}

void ValidateHunlGameConfig(const HunlGameConfig& config) {
  if (config.stack_bb <= 0.0) {
    throw std::invalid_argument("HUNL stack_bb must be > 0.");
  }
  if (config.small_blind_bb <= 0.0 || config.big_blind_bb <= 0.0) {
    throw std::invalid_argument("HUNL blind sizes must be > 0.");
  }
  if (config.small_blind_bb > config.big_blind_bb) {
    throw std::invalid_argument("HUNL requires small_blind_bb <= big_blind_bb.");
  }
  if (config.big_blind_bb >= config.stack_bb) {
    throw std::invalid_argument("HUNL requires stack_bb > big_blind_bb.");
  }
  if (config.action_config.max_raises_per_round == 0) {
    throw std::invalid_argument("HUNL max_raises_per_round must be > 0.");
  }
  if (config.action_config.min_bet_bb <= 0.0) {
    throw std::invalid_argument("HUNL min_bet_bb must be > 0.");
  }
}

HunlGame::HunlGame(HunlGameConfig config) : config_(std::move(config)) {
  ValidateHunlGameConfig(config_);
}

std::string HunlGame::Name() const {
  return "HunlAbstract";
}

std::unique_ptr<game::GameState> HunlGame::NewInitialState() const {
  return std::make_unique<HunlState>(&config_);
}

const HunlGameConfig& HunlGame::Config() const noexcept {
  return config_;
}

}  // namespace plarbius::games::hunl
