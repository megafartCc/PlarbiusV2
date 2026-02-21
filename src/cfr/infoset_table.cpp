#include "plarbius/cfr/infoset_table.hpp"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace plarbius::cfr {

InfosetNode& InfosetTable::GetOrCreate(const std::string& key, std::size_t action_count) {
  auto [it, inserted] = nodes_.try_emplace(key, InfosetNode{});
  InfosetNode& node = it->second;

  if (inserted) {
    node.regret_sum.assign(action_count, 0.0);
    node.strategy_sum.assign(action_count, 0.0);
    return node;
  }

  if (node.regret_sum.size() != action_count) {
    std::ostringstream oss;
    oss << "Action count mismatch for infoset '" << key << "': expected "
        << node.regret_sum.size() << ", got " << action_count;
    throw std::runtime_error(oss.str());
  }

  return node;
}

void InfosetTable::Put(const std::string& key, InfosetNode node) {
  if (node.regret_sum.size() != node.strategy_sum.size()) {
    std::ostringstream oss;
    oss << "Invalid infoset node for key '" << key
        << "': regret_sum and strategy_sum sizes differ.";
    throw std::runtime_error(oss.str());
  }
  nodes_[key] = std::move(node);
}

void InfosetTable::Clear() {
  nodes_.clear();
}

std::size_t InfosetTable::Size() const noexcept {
  return nodes_.size();
}

const std::unordered_map<std::string, InfosetNode>& InfosetTable::Nodes() const noexcept {
  return nodes_;
}

std::unordered_map<std::string, InfosetNode>& InfosetTable::MutableNodes() noexcept {
  return nodes_;
}

}  // namespace plarbius::cfr
