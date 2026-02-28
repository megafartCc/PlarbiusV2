#include "plarbius/games/hunl/hunl_game.hpp"

#include <atomic>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "plarbius/core/types.hpp"
#include "plarbius/eval/hand_eval.hpp"
#include "plarbius/game/chance_outcome.hpp"
#include "plarbius/game/game_state.hpp"
#include "plarbius/games/hunl/hunl_action_abstraction.hpp"
#include "plarbius/games/hunl/hunl_bucket_lookup.hpp"

namespace plarbius::games::hunl {

namespace {

constexpr std::size_t kStreetCount = 4;
constexpr double kEpsilon = 1e-9;
constexpr std::uint8_t kDeltaFlagTerminal = 1u << 0;
constexpr std::uint8_t kDeltaFlagDealt = 1u << 1;
constexpr std::array<double, 7> kSprBucketEdges = {0.01, 0.03, 0.08, 0.20, 0.50, 1.00, 2.00};
constexpr std::array<double, kHunlSprRawBinEdgeCount> kSprRawHistogramEdges = {
    0.02, 0.05, 0.08, 0.12, 0.16, 0.20, 0.30, 0.40, 0.50, 0.75,
    1.00, 1.50, 2.00, 3.00, 4.00, 6.00, 8.00, 12.0, 16.0, 32.0};

std::array<std::atomic<std::uint64_t>, kHunlSprRawStreetCount> g_spr_raw_samples{};
std::array<std::array<std::atomic<std::uint64_t>, kHunlSprRawBinCount>, kHunlSprRawStreetCount>
    g_spr_raw_histogram{};

void ValidateBucketModeForBuild(const HunlBucketConfig& config) {
  if (config.use_equity_buckets) {
    return;
  }
#if defined(NDEBUG)
  throw std::invalid_argument(
      "Hash bucket mode is debug-only and is blocked in release builds. "
      "Set use_equity_buckets=true.");
#else
  static bool warned_once = false;
  if (!warned_once) {
    std::cerr << "\n"
              << "############################################################\n"
              << "### WARNING: HASH BUCKET MODE ENABLED (DEBUG-ONLY MODE) ###\n"
              << "### This mode is for diagnostics only and not for models. ###\n"
              << "############################################################\n\n";
    warned_once = true;
  }
#endif
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

double StreetStrengthWeight(const HunlBucketConfig& config, std::size_t street) {
  switch (street) {
    case 0:
      return config.preflop_strength_weight;
    case 1:
      return config.flop_strength_weight;
    case 2:
      return config.turn_strength_weight;
    case 3:
      return config.river_strength_weight;
    default:
      throw std::out_of_range("Invalid street for strength weight.");
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

std::uint32_t BucketIndexFromFixedEdges(double value, const std::array<double, 7>& edges) {
  if (value <= 0.0) {
    return 0;
  }
  for (std::size_t i = 0; i < edges.size(); ++i) {
    if (value <= edges[i]) {
      return static_cast<std::uint32_t>(i);
    }
  }
  return static_cast<std::uint32_t>(edges.size());
}

std::uint32_t SnapToMergedLadder(double target,
                                 const std::vector<double>& first,
                                 const std::vector<double>& second) {
  std::size_t i = 0;
  std::size_t j = 0;
  double last = -1.0;
  std::uint32_t bucket = 0;
  std::uint32_t best_bucket = 0;
  double best_diff = std::numeric_limits<double>::infinity();

  while (i < first.size() || j < second.size()) {
    double value = 0.0;
    if (j >= second.size() || (i < first.size() && first[i] <= second[j])) {
      value = first[i++];
    } else {
      value = second[j++];
    }
    if (!(value > 0.0)) {
      continue;
    }
    if (last > 0.0 && std::fabs(value - last) < 1e-6) {
      continue;
    }
    last = value;
    bucket += 1;
    const double diff = std::fabs(target - value);
    if (diff < best_diff) {
      best_diff = diff;
      best_bucket = bucket;
    }
  }

  return best_bucket == 0 ? 1 : best_bucket;
}

const std::vector<double>& StreetBetSizesPot(const HunlActionAbstractionConfig& config,
                                             std::size_t street) {
  if (street == 1) {
    return config.flop_bet_sizes_pot;
  }
  if (street == 2) {
    return config.turn_bet_sizes_pot;
  }
  return config.river_bet_sizes_pot;
}

const std::vector<double>& StreetRaiseSizesPot(const HunlActionAbstractionConfig& config,
                                               std::size_t street) {
  if (street == 1) {
    return config.flop_raise_sizes_pot;
  }
  if (street == 2) {
    return config.turn_raise_sizes_pot;
  }
  return config.river_raise_sizes_pot;
}

std::size_t SprHistogramBin(double spr_value) {
  if (!(spr_value > 0.0)) {
    return 0;
  }
  for (std::size_t i = 0; i < kSprRawHistogramEdges.size(); ++i) {
    if (spr_value <= kSprRawHistogramEdges[i]) {
      return i;
    }
  }
  return kSprRawHistogramEdges.size();
}

void RecordSprRawSample(std::size_t street, double spr_value) {
  if (street >= kHunlSprRawStreetCount) {
    return;
  }
  const std::size_t bin = SprHistogramBin(spr_value);
  g_spr_raw_samples[street].fetch_add(1, std::memory_order_relaxed);
  g_spr_raw_histogram[street][bin].fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t FastHashString(const std::string& key) {
  std::uint64_t hash = 0x9e3779b97f4a7c15ULL;
  const std::uint8_t* data = reinterpret_cast<const std::uint8_t*>(key.data());
  std::size_t len = key.size();
  while (len >= 8) {
    std::uint64_t part;
    std::memcpy(&part, data, sizeof(std::uint64_t));
    hash ^= part + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    data += 8;
    len -= 8;
  }
  std::uint64_t tail = 0;
  for (std::size_t i = 0; i < len; ++i) {
    tail |= static_cast<std::uint64_t>(data[i]) << (i * 8);
  }
  hash ^= tail + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
  hash ^= (hash >> 33);
  hash *= 0xff51afd7ed558ccdULL;
  hash ^= (hash >> 33);
  hash *= 0xc4ceb9fe1a85ec53ULL;
  hash ^= (hash >> 33);
  return hash;
}

class HunlState final : public game::GameState {
 public:
  using Delta = HunlStateDelta;

  explicit HunlState(const HunlGameConfig* config) : config_(config) {
    if (config_ == nullptr) {
      throw std::invalid_argument("HunlState requires a valid config.");
    }
    if (config_->bucket_lookup == nullptr) {
      throw std::invalid_argument("HunlState requires a non-null bucket lookup table.");
    }
    if (config_->bucket_lookup->rows.size() != config_->bucket_config.chance_outcomes) {
      throw std::invalid_argument("HunlState bucket lookup row count does not match chance_outcomes.");
    }
    stack_remaining_ = {config_->stack_bb, config_->stack_bb};
    PostAnteIfEnabled();
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

  double PotBb() const {
    return committed_total_bb_[0] + committed_total_bb_[1];
  }

  std::vector<game::Action> LegalActions() const override {
    std::vector<game::Action> out;
    LegalActionsInPlace(out);
    return out;
  }

  void LegalActionsInPlace(std::vector<game::Action>& out) const {
    if (!dealt_ || terminal_) {
      out.clear();
      return;
    }
    RefreshLegalActionsIfNeeded();
    out = cached_legal_actions_;
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
    next->MarkInfosetDirty();
    return next;
  }

  void ApplyActionInPlace(const game::Action& action, Delta& delta) {
    SnapshotForUndo(delta);
    ApplyActionFromSnapshot(action);
  }

  void ApplyChanceInPlace(int outcome_id, Delta& delta) {
    SnapshotForUndo(delta);
    ApplyChanceFromSnapshot(outcome_id);
  }

  void SnapshotForUndo(Delta& delta) const { Snapshot(delta); }

  void ApplyActionFromSnapshot(const game::Action& action) { ApplyAction(action); }

  void ApplyChanceFromSnapshot(int outcome_id) {
    dealt_ = true;
    AssignAbstractDeal(static_cast<std::uint32_t>(outcome_id));
    MarkInfosetDirty();
  }

  void Undo(const Delta& delta) {
    stack_remaining_ = delta.stack_remaining;
    committed_total_bb_ = delta.committed_total_bb;
    committed_round_bb_ = delta.committed_round_bb;
    street_last_aggressor_ = delta.street_last_aggressor;
    street_bet_count_ = delta.street_bet_count;
    street_last_bet_bucket_ = delta.street_last_bet_bucket;
    street_facing_bet_ = delta.street_facing_bet;
    current_bet_bb_ = delta.current_bet_bb;
    raises_in_round_ = static_cast<std::size_t>(delta.raises_in_round);
    actions_in_round_ = static_cast<std::size_t>(delta.actions_in_round);
    street_ = static_cast<std::size_t>(delta.street);
    current_player_ = static_cast<PlayerId>(delta.current_player);
    terminal_ = (delta.flags & kDeltaFlagTerminal) != 0;
    dealt_ = (delta.flags & kDeltaFlagDealt) != 0;
    winner_ = delta.winner;
    history_.resize(delta.history_size);
    round_history_.resize(delta.round_history_size);
    MarkInfosetDirty();
    MarkHistoryDirty();
  }

  std::string InfosetKey(PlayerId player) const override {
    if (!dealt_ || terminal_) {
      return {};
    }
    if (player > 1) {
      throw std::out_of_range("Invalid player id for HUNL infoset.");
    }

    if (config_->infoset_key_mode == HunlInfosetKeyMode::kLegacy) {
      return InfosetKeyLegacy(player);
    }
    return InfosetKeyCompact(player);
  }

  std::string InfosetKeyCompact(PlayerId player) const {
    if (player > 1) {
      throw std::out_of_range("Invalid player id for HUNL infoset.");
    }

    if (!key_dirty_[player]) {
      return cached_infoset_key_[player];
    }

    if (prefix_dirty_[player] || bucket_dirty_) {
      RefreshBucketsIfNeeded();
      RefreshPrefix(player);
    }

    const std::string& history_text = CompactHistoryText();
    const std::string& prefix = cached_prefix_[player];

    constexpr std::size_t kKeyBufferSize = 256;
    if (prefix.size() + history_text.size() > kKeyBufferSize) {
      throw std::runtime_error("InfosetKey buffer overflow while writing history.");
    }

    char buf[kKeyBufferSize];
    char* p = buf;
    if (!prefix.empty()) {
      std::memcpy(p, prefix.data(), prefix.size());
      p += prefix.size();
    }
    if (!history_text.empty()) {
      std::memcpy(p, history_text.data(), history_text.size());
      p += history_text.size();
    }

    cached_infoset_key_[player].assign(buf, static_cast<std::size_t>(p - buf));
    cached_key_hash_[player] = FastHashString(cached_infoset_key_[player]);
    key_dirty_[player] = false;
    return cached_infoset_key_[player];
  }

  std::string InfosetKeyLegacy(PlayerId player) const {
    if (player > 1) {
      throw std::out_of_range("Invalid player id for HUNL infoset.");
    }

    if (!key_dirty_[player]) {
      return cached_infoset_key_[player];
    }

    RefreshLegalActionsIfNeeded();

    const double pot_bb = committed_total_bb_[0] + committed_total_bb_[1];
    const double to_call_bb = ToCallFor(player);
    const double stack_bb = std::max(0.0, stack_remaining_[player]);
    const double eff_bb = std::max(0.0, std::min(stack_remaining_[player], stack_remaining_[1 - player]));
    const std::uint32_t pot_bucket = CoarseBucket(
        pot_bb,
        config_->bucket_config.infoset_pot_bucket_width_bb,
        config_->bucket_config.infoset_pot_bucket_cap);
    const std::uint32_t to_call_bucket = CoarseBucket(
        to_call_bb,
        config_->bucket_config.infoset_to_call_bucket_width_bb,
        config_->bucket_config.infoset_to_call_bucket_cap);
    const std::uint32_t stack_bucket = CoarseBucket(
        stack_bb,
        config_->bucket_config.infoset_stack_bucket_width_bb,
        config_->bucket_config.infoset_stack_bucket_cap);
    const std::uint32_t eff_bucket =
        BucketIndexFromEdges(eff_bb, config_->stack_bucket_config.effective_stack_bb_edges);

    std::string key;
    key.reserve(256);
    key.append(std::to_string(player));
    key.append("|s");
    key.append(std::to_string(street_));
    key.append("|pr");
    key.append(std::to_string(private_bucket_[street_][player]));
    key.append("|pb");
    key.append(std::to_string(public_bucket_[street_]));
    key.append("|pot");
    key.append(std::to_string(pot_bucket));
    key.append("|tc");
    key.append(std::to_string(to_call_bucket));
    key.append("|stk");
    key.append(std::to_string(stack_bucket));
    key.append("|eff");
    key.append(std::to_string(eff_bucket));
    key.append("|r");
    key.append(std::to_string(raises_in_round_));
    key.push_back('|');
    key.append(LegacyHistoryText());
    key.append("|a");
    for (std::size_t i = 0; i < cached_legal_actions_.size(); ++i) {
      if (i > 0) {
        key.push_back(',');
      }
      key.append(DescribeHunlAction(cached_legal_actions_[i]));
    }

    cached_infoset_key_[player] = std::move(key);
    cached_key_hash_[player] = FastHashString(cached_infoset_key_[player]);
    key_dirty_[player] = false;
    return cached_infoset_key_[player];
  }

  std::uint64_t InfosetKeyHash(PlayerId player) const {
    if (player > 1) {
      throw std::out_of_range("Invalid player id for HUNL infoset hash.");
    }
    if (key_dirty_[player]) {
      InfosetKey(player);
    }
    return cached_key_hash_[player];
  }

 private:
  void Snapshot(Delta& delta) const {
    delta.stack_remaining = stack_remaining_;
    delta.committed_total_bb = committed_total_bb_;
    delta.committed_round_bb = committed_round_bb_;
    delta.street_last_aggressor = street_last_aggressor_;
    delta.street_bet_count = street_bet_count_;
    delta.street_last_bet_bucket = street_last_bet_bucket_;
    delta.street_facing_bet = street_facing_bet_;
    delta.current_bet_bb = current_bet_bb_;
    delta.raises_in_round = static_cast<std::uint16_t>(raises_in_round_);
    delta.actions_in_round = static_cast<std::uint16_t>(actions_in_round_);
    delta.street = static_cast<std::uint8_t>(street_);
    delta.current_player = static_cast<std::uint8_t>(current_player_);
    delta.winner = static_cast<std::int8_t>(winner_);
    delta.flags = 0;
    if (terminal_) {
      delta.flags |= kDeltaFlagTerminal;
    }
    if (dealt_) {
      delta.flags |= kDeltaFlagDealt;
    }
    if (history_.size() > std::numeric_limits<std::uint32_t>::max() ||
        round_history_.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error("HUNL history overflow while snapshotting state delta.");
    }
    delta.history_size = static_cast<std::uint32_t>(history_.size());
    delta.round_history_size = static_cast<std::uint32_t>(round_history_.size());
  }

  void PostBlind(PlayerId player, double amount_bb) {
    const double pay = std::min(amount_bb, stack_remaining_[player]);
    stack_remaining_[player] -= pay;
    committed_total_bb_[player] += pay;
    committed_round_bb_[player] += pay;
  }

  void PostAnteIfEnabled() {
    if (config_->ante_bb <= kEpsilon) {
      return;
    }
    PostBlind(kPlayer0, config_->ante_bb);
    PostBlind(kPlayer1, config_->ante_bb);
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
    MarkHistoryDirty();
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
    MarkHistoryDirty();
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
    // Depth-limited cutoff: if next street exceeds max_street, treat as terminal
    if (street_ + 1 > config_->max_street) {
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
    street_facing_bet_[street_] = 1;
    // Preflop SB completion (limp) should not close action; BB still has check/raise.
    const bool keep_preflop_open =
        street_ == 0 && current_player_ == kPlayer0 && actions_in_round_ == 0 &&
        raises_in_round_ == 0;
    ApplyPayment(current_player_, to_call);
    AppendRoundToken("c");
    actions_in_round_ += 1;
    if (keep_preflop_open && stack_remaining_[0] > kEpsilon && stack_remaining_[1] > kEpsilon) {
      current_player_ = kPlayer1;
      return;
    }
    EndRound();
  }

  void ApplyFold() {
    if (ToCallFor(current_player_) <= kEpsilon) {
      throw std::runtime_error("Fold is invalid when there is no outstanding bet.");
    }
    street_facing_bet_[street_] = 1;
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
    street_facing_bet_[street_] = 1;
    street_last_aggressor_[street_] = static_cast<std::uint8_t>(current_player_ + 1);
    if (street_bet_count_[street_] < std::numeric_limits<std::uint8_t>::max()) {
      street_bet_count_[street_] += 1;
    }
    street_last_bet_bucket_[street_] = action.abstraction_id;
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
        break;
      case game::ActionType::kCall:
        ApplyCall();
        break;
      case game::ActionType::kFold:
        ApplyFold();
        break;
      case game::ActionType::kBet:
        ApplyBetOrRaise(action);
        break;
      default:
        throw std::runtime_error("Unknown action in HUNL state.");
    }
    MarkInfosetDirty();
  }

  void AssignAbstractDeal(std::uint32_t outcome_id) {
    const auto& row = config_->bucket_lookup->rows[outcome_id];
    private_bucket_ = row.private_bucket;
    public_bucket_  = row.public_bucket;
    // Store actual cards so ShowdownScore can use real hand ranks.
    if (row.has_real_cards) {
      deal_hero_  = {static_cast<int>(row.hero_hole[0]), static_cast<int>(row.hero_hole[1])};
      deal_opp_   = {static_cast<int>(row.opp_hole[0]),  static_cast<int>(row.opp_hole[1])};
      deal_board_ = {static_cast<int>(row.board[0]), static_cast<int>(row.board[1]),
                     static_cast<int>(row.board[2]), static_cast<int>(row.board[3]),
                     static_cast<int>(row.board[4])};
      has_real_deal_ = true;
    } else {
      has_real_deal_ = false;
    }
  }

  double NormalizedBucket(std::uint32_t bucket, std::uint32_t count) const {
    if (count <= 1) return 0.0;
    return static_cast<double>(bucket) / static_cast<double>(count - 1);
  }

  // ShowdownScore: uses real hand evaluator when cards are available (EMD/equity modes),
  // falls back to bucket proxy for hash-bucket mode.
  double ShowdownScore(PlayerId player) const {
    if (has_real_deal_) {
      // Real hand evaluation — actual 7-card Omaha-style best-5.
      const std::array<int,2>& hole = (player == 0) ? deal_hero_ : deal_opp_;
      // Build 7-card array from the hole cards + board.
      const std::array<int,7> seven = {
          hole[0], hole[1],
          deal_board_[0], deal_board_[1], deal_board_[2],
          deal_board_[3], deal_board_[4]};
      return static_cast<double>(eval::ScoreSevenCardHand(seven));
    }
    // Legacy bucket-proxy fallback.
    const double private_mix = config_->bucket_config.private_bucket_mix;
    const double public_mix  = 1.0 - private_mix;
    double score = 0.0;
    for (std::size_t street = 0; street < kStreetCount; ++street) {
      const double pp = NormalizedBucket(private_bucket_[street][player],
                                          PrivateBucketCount(config_->bucket_config, street));
      const double pub = NormalizedBucket(public_bucket_[street],
                                           PublicBucketCount(config_->bucket_config, street));
      score += StreetStrengthWeight(config_->bucket_config, street) *
               (private_mix * pp + public_mix * pub);
    }
    return score;
  }

  void MarkInfosetDirty() {
    if (key_dirty_[0] && key_dirty_[1] && prefix_dirty_[0] && prefix_dirty_[1] && bucket_dirty_ &&
        legal_actions_dirty_) {
      return;
    }
    key_dirty_[0] = true;
    key_dirty_[1] = true;
    prefix_dirty_[0] = true;
    prefix_dirty_[1] = true;
    bucket_dirty_ = true;
    legal_actions_dirty_ = true;
  }

  void MarkHistoryDirty() {
    if (compact_history_dirty_ && legacy_history_dirty_ && key_dirty_[0] && key_dirty_[1]) {
      return;
    }
    compact_history_dirty_ = true;
    legacy_history_dirty_ = true;
    key_dirty_[0] = true;
    key_dirty_[1] = true;
  }

  const std::string& CompactHistoryText() const {
    if (!compact_history_dirty_) {
      return cached_compact_history_text_;
    }
    char buf[192];
    char* p = buf;
    char* const end = buf + sizeof(buf);

    const auto append_literal = [&](const char* lit) {
      while (*lit != '\0') {
        if (p >= end) {
          throw std::runtime_error("InfosetKey history abstraction buffer overflow.");
        }
        *p++ = *lit++;
      }
    };
    const auto append_uint = [&](std::uint32_t value) {
      const auto res = std::to_chars(p, end, value);
      if (res.ec != std::errc()) {
        throw std::runtime_error("InfosetKey history abstraction integer overflow.");
      }
      p = res.ptr;
    };

    for (std::size_t street = 0; street < kStreetCount; ++street) {
      if (street > 0) {
        append_literal(".");
      }
      append_literal("h");
      append_uint(static_cast<std::uint32_t>(street));
      append_literal("a");
      append_uint(street_last_aggressor_[street]);
      append_literal("b");
      append_uint(street_bet_count_[street]);
      append_literal("l");
      append_uint(street_last_bet_bucket_[street]);
      append_literal("f");
      append_uint(street_facing_bet_[street]);
    }
    cached_compact_history_text_.assign(buf, static_cast<std::size_t>(p - buf));
    compact_history_dirty_ = false;
    return cached_compact_history_text_;
  }

  const std::string& LegacyHistoryText() const {
    if (!legacy_history_dirty_) {
      return cached_legacy_history_text_;
    }
    if (history_.empty() && round_history_.empty()) {
      cached_legacy_history_text_.assign("_");
    } else if (history_.empty()) {
      cached_legacy_history_text_ = round_history_;
    } else if (round_history_.empty()) {
      cached_legacy_history_text_.clear();
      cached_legacy_history_text_.reserve(history_.size() + 2);
      cached_legacy_history_text_.append(history_);
      cached_legacy_history_text_.append("/_");
    } else {
      cached_legacy_history_text_.clear();
      cached_legacy_history_text_.reserve(history_.size() + 1 + round_history_.size());
      cached_legacy_history_text_.append(history_);
      cached_legacy_history_text_.push_back('/');
      cached_legacy_history_text_.append(round_history_);
    }
    legacy_history_dirty_ = false;
    return cached_legacy_history_text_;
  }

  void RefreshLegalActionsIfNeeded() const {
    if (!legal_actions_dirty_) {
      return;
    }
    cached_legal_actions_.clear();
    if (!dealt_ || terminal_) {
      legal_actions_dirty_ = false;
      return;
    }
    HunlLegalActionContext context;
    context.street = street_;
    context.facing_bet = ToCallFor(current_player_) > kEpsilon;
    context.pot_bb = committed_total_bb_[0] + committed_total_bb_[1];
    context.to_call_bb = ToCallFor(current_player_);
    context.stack_to_act_bb = stack_remaining_[current_player_];
    context.raises_in_round = raises_in_round_;
    BuildHunlLegalActions(config_->action_config, context, cached_legal_actions_);
    legal_actions_dirty_ = false;
  }

  void RefreshBucketsIfNeeded() const {
    if (!bucket_dirty_) {
      return;
    }

    const double pot_bb = committed_total_bb_[0] + committed_total_bb_[1];
    const double safe_pot_bb = std::max(kEpsilon, pot_bb);
    const PlayerId to_act = current_player_;
    const PlayerId other = 1 - to_act;
    // Use effective stack from the acting player's perspective, including any
    // chips already committed by the opponent in the outstanding bet.
    const double to_call_bb = ToCallFor(to_act);
    const double eff_stack_bb = std::max(
        0.0,
        std::min(stack_remaining_[to_act], stack_remaining_[other] + std::max(0.0, to_call_bb)));
    const double spr = eff_stack_bb / safe_pot_bb;
    cached_spr_bucket_ = BucketIndexFromFixedEdges(spr, kSprBucketEdges);
    RecordSprRawSample(street_, spr);

    const auto facing_bucket_for = [&](PlayerId player) -> std::uint32_t {
      const double to_call_bb = ToCallFor(player);
      if (to_call_bb <= kEpsilon) {
        return 0;
      }
      if (street_ == 0) {
        return SnapToMergedLadder(
            to_call_bb,
            config_->action_config.preflop_open_sizes_bb,
            config_->action_config.preflop_raise_sizes_bb);
      }
      return SnapToMergedLadder(
          to_call_bb / safe_pot_bb,
          StreetBetSizesPot(config_->action_config, street_),
          StreetRaiseSizesPot(config_->action_config, street_));
    };

    cached_facing_bucket_[0] = facing_bucket_for(kPlayer0);
    cached_facing_bucket_[1] = facing_bucket_for(kPlayer1);

    if (config_->stack_bucket_config.include_ante_bucket) {
      const double ante_bb = config_->ante_bb / std::max(kEpsilon, config_->big_blind_bb);
      cached_ante_bucket_ =
          BucketIndexFromEdges(ante_bb, config_->stack_bucket_config.ante_bb_edges);
    } else {
      cached_ante_bucket_ = 0;
    }

    bucket_dirty_ = false;
  }

  void RefreshPrefix(PlayerId player) const {
    if (player > 1) {
      throw std::out_of_range("Invalid player id for HUNL prefix refresh.");
    }
    RefreshLegalActionsIfNeeded();

    char buf[160];
    char* p = buf;
    const auto end = buf + sizeof(buf);

    const auto append_literal = [&](const char* lit) {
      while (*lit && p < end) {
        *p++ = *lit++;
      }
    };
    const auto append_uint = [&](std::uint32_t value) {
      auto res = std::to_chars(p, end, value);
      if (res.ec == std::errc()) {
        p = res.ptr;
      } else {
        throw std::runtime_error("InfosetKey buffer overflow during integer append.");
      }
    };

    append_uint(static_cast<std::uint32_t>(player));
    append_literal("|s");
    append_uint(static_cast<std::uint32_t>(street_));
    append_literal("|pr");
    append_uint(private_bucket_[street_][player]);
    append_literal("|pb");
    append_uint(public_bucket_[street_]);
    append_literal("|spr");
    append_uint(cached_spr_bucket_);
    append_literal("|fb");
    append_uint(cached_facing_bucket_[player]);
    if (config_->stack_bucket_config.include_ante_bucket) {
      append_literal("|ante");
      append_uint(cached_ante_bucket_);
    }
    append_literal("|r");
    append_uint(static_cast<std::uint32_t>(raises_in_round_));
    append_literal("|n");
    append_uint(static_cast<std::uint32_t>(cached_legal_actions_.size()));
    append_literal("|");

    cached_prefix_[player].assign(buf, static_cast<std::size_t>(p - buf));
    prefix_dirty_[player] = false;
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
  // Actual hole cards and board assigned at deal time (valid when has_real_deal_=true).
  std::array<int, 2> deal_hero_  = {-1, -1};
  std::array<int, 2> deal_opp_   = {-1, -1};
  std::array<int, 5> deal_board_ = {-1, -1, -1, -1, -1};
  bool has_real_deal_ = false;
  std::array<std::uint8_t, kStreetCount> street_last_aggressor_ = {};
  std::array<std::uint8_t, kStreetCount> street_bet_count_ = {};
  std::array<std::uint16_t, kStreetCount> street_last_bet_bucket_ = {};
  std::array<std::uint8_t, kStreetCount> street_facing_bet_ = {};
  std::string history_;
  std::string round_history_;
  int winner_ = -2;
  mutable std::array<std::string, 2> cached_infoset_key_{};
  mutable std::array<bool, 2> key_dirty_{{true, true}};
  mutable std::array<std::uint64_t, 2> cached_key_hash_{{0, 0}};
  mutable std::array<std::string, 2> cached_prefix_{};
  mutable std::array<bool, 2> prefix_dirty_{{true, true}};
  mutable std::uint32_t cached_spr_bucket_ = 0;
  mutable std::array<std::uint32_t, 2> cached_facing_bucket_{{0, 0}};
  mutable std::uint32_t cached_ante_bucket_ = 0;
  mutable bool bucket_dirty_ = true;
  mutable std::string cached_compact_history_text_;
  mutable bool compact_history_dirty_ = true;
  mutable std::string cached_legacy_history_text_;
  mutable bool legacy_history_dirty_ = true;
  mutable std::vector<game::Action> cached_legal_actions_;
  mutable bool legal_actions_dirty_ = true;
};

}  // namespace

HunlGameConfig DefaultHunlGameConfig() {
  return HunlGameConfig{};
}

void ResetHunlSprRawTelemetry() {
  for (std::size_t street = 0; street < kHunlSprRawStreetCount; ++street) {
    g_spr_raw_samples[street].store(0, std::memory_order_relaxed);
    for (std::size_t bin = 0; bin < kHunlSprRawBinCount; ++bin) {
      g_spr_raw_histogram[street][bin].store(0, std::memory_order_relaxed);
    }
  }
}

HunlSprRawTelemetrySnapshot SnapshotHunlSprRawTelemetry() {
  HunlSprRawTelemetrySnapshot snapshot;
  snapshot.bin_edges = kSprRawHistogramEdges;
  for (std::size_t street = 0; street < kHunlSprRawStreetCount; ++street) {
    snapshot.streets[street].samples =
        g_spr_raw_samples[street].load(std::memory_order_relaxed);
    for (std::size_t bin = 0; bin < kHunlSprRawBinCount; ++bin) {
      snapshot.streets[street].histogram[bin] =
          g_spr_raw_histogram[street][bin].load(std::memory_order_relaxed);
    }
  }
  return snapshot;
}

void ValidateHunlGameConfig(const HunlGameConfig& config) {
  ValidateHunlStackBucketConfig(config.stack_bucket_config);
  if (config.bucket_lookup != nullptr) {
    if (config.bucket_lookup->rows.size() != config.bucket_config.chance_outcomes) {
      throw std::invalid_argument("bucket_lookup rows must match chance_outcomes.");
    }
  }
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
  if (config.ante_bb < 0.0) {
    throw std::invalid_argument("HUNL ante_bb must be >= 0.");
  }
  if (config.ante_bb >= config.stack_bb) {
    throw std::invalid_argument("HUNL requires ante_bb < stack_bb.");
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
  ValidateBucketModeForBuild(config_.bucket_config);
  if (config_.bucket_lookup == nullptr) {
    if (config_.bucket_config.use_emd_buckets) {
      config_.bucket_lookup =
          std::make_shared<HunlBucketLookupTable>(BuildHunlEmdBucketLookup(config_.bucket_config));
    } else if (config_.bucket_config.use_equity_buckets) {
      config_.bucket_lookup =
          std::make_shared<HunlBucketLookupTable>(BuildHunlEquityBucketLookup(config_.bucket_config));
    } else {
      config_.bucket_lookup =
          std::make_shared<HunlBucketLookupTable>(BuildHunlHashBucketLookup(config_.bucket_config));
    }
  }
  ValidateHunlBucketLookup(*config_.bucket_lookup, config_.bucket_config);
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

bool HunlSupportsApplyUndo(const game::GameState& state) {
  return dynamic_cast<const HunlState*>(&state) != nullptr;
}

bool HunlApplyActionInPlace(game::GameState& state,
                            const game::Action& action,
                            HunlStateDelta& delta) {
  auto* hs = dynamic_cast<HunlState*>(&state);
  if (hs == nullptr) {
    return false;
  }
  hs->ApplyActionInPlace(action, delta);
  return true;
}

void HunlSnapshotForUndoUnchecked(const game::GameState& state, HunlStateDelta& delta) {
  static_cast<const HunlState&>(state).SnapshotForUndo(delta);
}

void HunlApplyActionFromSnapshotUnchecked(game::GameState& state, const game::Action& action) {
  static_cast<HunlState&>(state).ApplyActionFromSnapshot(action);
}

void HunlApplyActionInPlaceUnchecked(game::GameState& state,
                                     const game::Action& action,
                                     HunlStateDelta& delta) {
  static_cast<HunlState&>(state).ApplyActionInPlace(action, delta);
}

bool HunlApplyChanceInPlace(game::GameState& state, int outcome_id, HunlStateDelta& delta) {
  auto* hs = dynamic_cast<HunlState*>(&state);
  if (hs == nullptr) {
    return false;
  }
  hs->ApplyChanceInPlace(outcome_id, delta);
  return true;
}

void HunlApplyChanceFromSnapshotUnchecked(game::GameState& state, int outcome_id) {
  static_cast<HunlState&>(state).ApplyChanceFromSnapshot(outcome_id);
}

void HunlApplyChanceInPlaceUnchecked(game::GameState& state, int outcome_id, HunlStateDelta& delta) {
  static_cast<HunlState&>(state).ApplyChanceInPlace(outcome_id, delta);
}

bool HunlUndo(game::GameState& state, const HunlStateDelta& delta) {
  auto* hs = dynamic_cast<HunlState*>(&state);
  if (hs == nullptr) {
    return false;
  }
  hs->Undo(delta);
  return true;
}

void HunlUndoUnchecked(game::GameState& state, const HunlStateDelta& delta) {
  static_cast<HunlState&>(state).Undo(delta);
}

std::uint64_t HunlInfosetKeyHash(const game::GameState& state, PlayerId player) {
  const auto* hs = dynamic_cast<const HunlState*>(&state);
  if (hs == nullptr) {
    return 0;
  }
  return hs->InfosetKeyHash(player);
}

std::uint64_t HunlInfosetKeyHashUnchecked(const game::GameState& state, PlayerId player) {
  return static_cast<const HunlState&>(state).InfosetKeyHash(player);
}

bool HunlLegalActionsInPlace(const game::GameState& state, std::vector<game::Action>& out) {
  const auto* hs = dynamic_cast<const HunlState*>(&state);
  if (hs == nullptr) {
    return false;
  }
  hs->LegalActionsInPlace(out);
  return true;
}

void HunlLegalActionsInPlaceUnchecked(const game::GameState& state, std::vector<game::Action>& out) {
  static_cast<const HunlState&>(state).LegalActionsInPlace(out);
}

double HunlPotBb(const game::GameState& state) {
  const auto* hs = dynamic_cast<const HunlState*>(&state);
  if (hs == nullptr) {
    return 0.0;
  }
  return hs->PotBb();
}

}  // namespace plarbius::games::hunl
