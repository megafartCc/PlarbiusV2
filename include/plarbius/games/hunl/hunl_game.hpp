#pragma once

#include <string>

#include "plarbius/game/game.hpp"
#include "plarbius/games/hunl/hunl_action_abstraction.hpp"
#include "plarbius/games/hunl/hunl_bucket_config.hpp"

namespace plarbius::games::hunl {

struct HunlGameConfig {
  HunlBucketConfig bucket_config = DefaultHunlBucketConfig();
  HunlActionAbstractionConfig action_config = DefaultHunlActionAbstractionConfig();
  double stack_bb = 100.0;
  double small_blind_bb = 0.5;
  double big_blind_bb = 1.0;
};

HunlGameConfig DefaultHunlGameConfig();
void ValidateHunlGameConfig(const HunlGameConfig& config);

class HunlGame final : public game::Game {
 public:
  explicit HunlGame(HunlGameConfig config = DefaultHunlGameConfig());

  std::string Name() const override;
  std::unique_ptr<game::GameState> NewInitialState() const override;

  [[nodiscard]] const HunlGameConfig& Config() const noexcept;

 private:
  HunlGameConfig config_;
};

}  // namespace plarbius::games::hunl
