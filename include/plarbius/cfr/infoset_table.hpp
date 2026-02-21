#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace plarbius::cfr {

struct InfosetNode {
  std::vector<double> regret_sum;
  std::vector<double> strategy_sum;
};

class InfosetTable {
 public:
  InfosetNode& GetOrCreate(const std::string& key, std::size_t action_count);
  void Put(const std::string& key, InfosetNode node);
  void Clear();

  [[nodiscard]] std::size_t Size() const noexcept;
  [[nodiscard]] const std::unordered_map<std::string, InfosetNode>& Nodes() const noexcept;

 private:
  std::unordered_map<std::string, InfosetNode> nodes_;
};

}  // namespace plarbius::cfr
