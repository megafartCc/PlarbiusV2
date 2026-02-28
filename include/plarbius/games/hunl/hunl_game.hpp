#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "plarbius/game/game.hpp"
#include "plarbius/games/hunl/hunl_action_abstraction.hpp"
#include "plarbius/games/hunl/hunl_bucket_config.hpp"
#include "plarbius/games/hunl/hunl_bucket_lookup.hpp"
#include "plarbius/games/hunl/hunl_stack_bucket_config.hpp"

namespace plarbius::games::hunl {

enum class HunlInfosetKeyMode : std::uint8_t {
  kCompact = 0,
  kLegacy = 1,
};

struct HunlStateDelta {
  std::array<double, 2> stack_remaining;
  std::array<double, 2> committed_total_bb;
  std::array<double, 2> committed_round_bb;
  std::array<std::uint8_t, 4> street_last_aggressor{};
  std::array<std::uint8_t, 4> street_bet_count{};
  std::array<std::uint16_t, 4> street_last_bet_bucket{};
  std::array<std::uint8_t, 4> street_facing_bet{};
  double current_bet_bb;
  std::uint32_t history_size;
  std::uint32_t round_history_size;
  std::uint16_t raises_in_round;
  std::uint16_t actions_in_round;
  std::uint8_t street;
  std::uint8_t current_player;
  std::int8_t winner;
  std::uint8_t flags;
};

struct HunlGameConfig {
  HunlBucketConfig bucket_config = DefaultHunlBucketConfig();
  HunlActionAbstractionConfig action_config = DefaultHunlActionAbstractionConfig();
  HunlStackBucketConfig stack_bucket_config = DefaultHunlStackBucketConfig();
  HunlInfosetKeyMode infoset_key_mode = HunlInfosetKeyMode::kCompact;
  std::shared_ptr<const HunlBucketLookupTable> bucket_lookup;
  double stack_bb = 100.0;
  double small_blind_bb = 0.5;
  double big_blind_bb = 1.0;
  double ante_bb = 0.0;
  std::uint8_t max_street = 3;  // 0=preflop, 1=flop, 2=turn, 3=river (full)
};

inline constexpr std::size_t kHunlSprRawStreetCount = 4;
inline constexpr std::size_t kHunlSprRawBinEdgeCount = 20;
inline constexpr std::size_t kHunlSprRawBinCount = kHunlSprRawBinEdgeCount + 1;

struct HunlSprRawStreetStats {
  std::uint64_t samples = 0;
  std::array<std::uint64_t, kHunlSprRawBinCount> histogram{};
};

struct HunlSprRawTelemetrySnapshot {
  std::array<double, kHunlSprRawBinEdgeCount> bin_edges{};
  std::array<HunlSprRawStreetStats, kHunlSprRawStreetCount> streets{};
};

HunlGameConfig DefaultHunlGameConfig();
void ValidateHunlGameConfig(const HunlGameConfig& config);
void ResetHunlSprRawTelemetry();
HunlSprRawTelemetrySnapshot SnapshotHunlSprRawTelemetry();

class HunlGame final : public game::Game {
 public:
  explicit HunlGame(HunlGameConfig config = DefaultHunlGameConfig());

  std::string Name() const override;
  std::unique_ptr<game::GameState> NewInitialState() const override;

  [[nodiscard]] const HunlGameConfig& Config() const noexcept;

 private:
  HunlGameConfig config_;
};

bool HunlSupportsApplyUndo(const game::GameState& state);
bool HunlApplyActionInPlace(game::GameState& state,
                            const game::Action& action,
                            HunlStateDelta& delta);
void HunlSnapshotForUndoUnchecked(const game::GameState& state, HunlStateDelta& delta);
void HunlApplyActionFromSnapshotUnchecked(game::GameState& state, const game::Action& action);
void HunlApplyActionInPlaceUnchecked(game::GameState& state,
                                     const game::Action& action,
                                     HunlStateDelta& delta);
bool HunlApplyChanceInPlace(game::GameState& state, int outcome_id, HunlStateDelta& delta);
void HunlApplyChanceFromSnapshotUnchecked(game::GameState& state, int outcome_id);
void HunlApplyChanceInPlaceUnchecked(game::GameState& state, int outcome_id, HunlStateDelta& delta);
bool HunlUndo(game::GameState& state, const HunlStateDelta& delta);
void HunlUndoUnchecked(game::GameState& state, const HunlStateDelta& delta);
std::uint64_t HunlInfosetKeyHash(const game::GameState& state, PlayerId player);
std::uint64_t HunlInfosetKeyHashUnchecked(const game::GameState& state, PlayerId player);
bool HunlLegalActionsInPlace(const game::GameState& state, std::vector<game::Action>& out);
void HunlLegalActionsInPlaceUnchecked(const game::GameState& state,
                                      std::vector<game::Action>& out);
double HunlPotBb(const game::GameState& state);

}  // namespace plarbius::games::hunl
