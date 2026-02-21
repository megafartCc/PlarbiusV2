#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "plarbius/cfr/infoset_table.hpp"

namespace plarbius::policy {

using PolicyTable = std::unordered_map<std::string, std::vector<double>>;

PolicyTable BuildAveragePolicy(const cfr::InfosetTable& table);
void SavePolicy(const PolicyTable& policy, const std::string& path);
PolicyTable LoadPolicy(const std::string& path);
std::vector<double> GetActionDistribution(const PolicyTable& policy,
                                          const std::string& infoset_key,
                                          std::size_t action_count);

}  // namespace plarbius::policy

