#include "plarbius/policy/policy_io.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "plarbius/cfr/regret_matcher.hpp"

namespace plarbius::policy {

namespace {

constexpr const char* kFormatMagic = "PLARBIUS_POLICY_V1";

std::string JoinDoubles(const std::vector<double>& values) {
  std::ostringstream oss;
  oss << std::setprecision(17);
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      oss << ',';
    }
    oss << values[i];
  }
  return oss.str();
}

std::vector<double> SplitDoubles(const std::string& input, std::size_t expected_size) {
  std::vector<double> values;
  values.reserve(expected_size);

  std::size_t start = 0;
  while (start <= input.size()) {
    const std::size_t comma = input.find(',', start);
    const std::size_t end = comma == std::string::npos ? input.size() : comma;
    const std::string token = input.substr(start, end - start);
    if (token.empty()) {
      throw std::runtime_error("Malformed policy vector.");
    }
    values.push_back(std::stod(token));
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }

  if (values.size() != expected_size) {
    std::ostringstream oss;
    oss << "Policy vector size mismatch. Expected " << expected_size << ", got " << values.size();
    throw std::runtime_error(oss.str());
  }
  return values;
}

std::vector<std::string> SplitTab(const std::string& line) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= line.size()) {
    const std::size_t tab = line.find('\t', start);
    const std::size_t end = tab == std::string::npos ? line.size() : tab;
    parts.push_back(line.substr(start, end - start));
    if (tab == std::string::npos) {
      break;
    }
    start = tab + 1;
  }
  return parts;
}

}  // namespace

PolicyTable BuildAveragePolicy(const cfr::InfosetTable& table) {
  PolicyTable policy;
  policy.reserve(table.Nodes().size());
  for (const auto& [key, node] : table.Nodes()) {
    policy.emplace(key, cfr::RegretMatcher::Normalize(node.strategy_sum));
  }
  return policy;
}

void SavePolicy(const PolicyTable& policy, const std::string& path) {
  const std::filesystem::path fs_path(path);
  if (fs_path.has_parent_path()) {
    std::filesystem::create_directories(fs_path.parent_path());
  }

  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("Failed to open policy for writing: " + path);
  }

  out << kFormatMagic << '\n';

  std::vector<std::pair<std::string, std::vector<double>>> rows(policy.begin(), policy.end());
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return left.first < right.first;
  });

  for (const auto& [key, probs] : rows) {
    out << key << '\t' << probs.size() << '\t' << JoinDoubles(probs) << '\n';
  }
}

PolicyTable LoadPolicy(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("Failed to open policy for reading: " + path);
  }

  std::string magic;
  if (!std::getline(in, magic)) {
    throw std::runtime_error("Policy file is empty: " + path);
  }
  if (magic != kFormatMagic) {
    throw std::runtime_error("Unsupported policy format: " + magic);
  }

  PolicyTable policy;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    const auto parts = SplitTab(line);
    if (parts.size() != 3) {
      throw std::runtime_error("Malformed policy row: " + line);
    }
    const std::size_t action_count = static_cast<std::size_t>(std::stoull(parts[1]));
    auto probs = SplitDoubles(parts[2], action_count);
    probs = cfr::RegretMatcher::Normalize(probs);
    policy[parts[0]] = std::move(probs);
  }

  return policy;
}

std::vector<double> GetActionDistribution(const PolicyTable& policy,
                                          const std::string& infoset_key,
                                          std::size_t action_count) {
  const auto it = policy.find(infoset_key);
  if (it == policy.end()) {
    std::vector<double> uniform(action_count, 0.0);
    if (action_count > 0) {
      const double p = 1.0 / static_cast<double>(action_count);
      std::fill(uniform.begin(), uniform.end(), p);
    }
    return uniform;
  }
  if (it->second.size() != action_count) {
    std::ostringstream oss;
    oss << "Policy action count mismatch for infoset '" << infoset_key << "': expected "
        << action_count << ", got " << it->second.size();
    throw std::runtime_error(oss.str());
  }
  return cfr::RegretMatcher::Normalize(it->second);
}

}  // namespace plarbius::policy

