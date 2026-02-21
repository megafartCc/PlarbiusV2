#pragma once

#include <cstdint>
#include <functional>

#include "plarbius/cfr/infoset_table.hpp"

namespace plarbius::cfr {

using TrainingMetricsCallback = std::function<void(std::uint64_t, const InfosetTable&)>;

}  // namespace plarbius::cfr

