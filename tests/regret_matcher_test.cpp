#include <array>
#include <cmath>
#include <iostream>

#include "plarbius/cfr/regret_matcher.hpp"

namespace {

bool NearlyEqual(double a, double b, double epsilon = 1e-9) {
  return std::fabs(a - b) <= epsilon;
}

}  // namespace

int main() {
  {
    const std::array<double, 3> regrets = {0.0, 0.0, 0.0};
    const auto strategy = plarbius::cfr::RegretMatcher::ComputeStrategyPlus(regrets);
    if (!NearlyEqual(strategy[0], 1.0 / 3.0) || !NearlyEqual(strategy[1], 1.0 / 3.0) ||
        !NearlyEqual(strategy[2], 1.0 / 3.0)) {
      std::cerr << "Uniform strategy test failed.\n";
      return 1;
    }
  }

  {
    const std::array<double, 3> regrets = {-5.0, 2.0, 1.0};
    const auto strategy = plarbius::cfr::RegretMatcher::ComputeStrategyPlus(regrets);
    if (!NearlyEqual(strategy[0], 0.0) || !NearlyEqual(strategy[1], 2.0 / 3.0) ||
        !NearlyEqual(strategy[2], 1.0 / 3.0)) {
      std::cerr << "Positive-regret strategy test failed.\n";
      return 1;
    }
  }

  {
    const std::array<double, 2> values = {2.0, 6.0};
    const auto normalized = plarbius::cfr::RegretMatcher::Normalize(values);
    if (!NearlyEqual(normalized[0], 0.25) || !NearlyEqual(normalized[1], 0.75)) {
      std::cerr << "Normalization test failed.\n";
      return 1;
    }
  }

  return 0;
}

