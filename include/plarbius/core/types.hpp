#pragma once

#include <cstddef>
#include <cstdint>

namespace plarbius {

using PlayerId = std::uint8_t;

constexpr PlayerId kPlayer0 = 0;
constexpr PlayerId kPlayer1 = 1;
constexpr std::size_t kNumPlayers = 2;

}  // namespace plarbius

