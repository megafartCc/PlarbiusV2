#pragma once

#include <span>
#include <vector>

namespace plarbius::cfr {

class RegretMatcher {
 public:
  static std::vector<double> ComputeStrategyPlus(std::span<const double> regrets);
  static std::vector<double> Normalize(std::span<const double> values);
};

}  // namespace plarbius::cfr

