#include "plarbius/cfr/regret_matcher.hpp"

#include <algorithm>

namespace plarbius::cfr {

std::vector<double> RegretMatcher::ComputeStrategyPlus(std::span<const double> regrets) {
  std::vector<double> positive(regrets.size(), 0.0);
  double sum = 0.0;

  for (std::size_t i = 0; i < regrets.size(); ++i) {
    positive[i] = std::max(0.0, regrets[i]);
    sum += positive[i];
  }

  if (sum <= 0.0) {
    const double uniform = regrets.empty() ? 0.0 : 1.0 / static_cast<double>(regrets.size());
    std::fill(positive.begin(), positive.end(), uniform);
    return positive;
  }

  for (double& value : positive) {
    value /= sum;
  }
  return positive;
}

std::vector<double> RegretMatcher::Normalize(std::span<const double> values) {
  std::vector<double> normalized(values.begin(), values.end());
  double sum = 0.0;
  for (double value : normalized) {
    sum += value;
  }

  if (sum <= 0.0) {
    const double uniform = normalized.empty() ? 0.0 : 1.0 / static_cast<double>(normalized.size());
    std::fill(normalized.begin(), normalized.end(), uniform);
    return normalized;
  }

  for (double& value : normalized) {
    value /= sum;
  }
  return normalized;
}

}  // namespace plarbius::cfr

