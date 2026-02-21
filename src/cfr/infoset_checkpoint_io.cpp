#include "plarbius/cfr/infoset_checkpoint_io.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace plarbius::cfr {

namespace {

constexpr const char* kFormatMagic = "PLARBIUS_INFOSET_V1";

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
      throw std::runtime_error("Malformed numeric vector in checkpoint.");
    }
    values.push_back(std::stod(token));
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }

  if (values.size() != expected_size) {
    std::ostringstream oss;
    oss << "Checkpoint vector size mismatch. Expected " << expected_size << ", got "
        << values.size();
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

void InfosetCheckpointIo::Save(const InfosetTable& table, const std::string& path) {
  const std::filesystem::path fs_path(path);
  if (fs_path.has_parent_path()) {
    std::filesystem::create_directories(fs_path.parent_path());
  }

  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("Failed to open checkpoint for writing: " + path);
  }

  out << kFormatMagic << '\n';

  std::vector<std::pair<std::string, InfosetNode>> rows(table.Nodes().begin(), table.Nodes().end());
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return left.first < right.first;
  });

  for (const auto& [key, node] : rows) {
    if (node.regret_sum.size() != node.strategy_sum.size()) {
      throw std::runtime_error("Invalid infoset node in table: " + key);
    }
    out << key << '\t' << node.regret_sum.size() << '\t' << JoinDoubles(node.regret_sum) << '\t'
        << JoinDoubles(node.strategy_sum) << '\n';
  }
}

void InfosetCheckpointIo::Load(InfosetTable& table, const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("Failed to open checkpoint for reading: " + path);
  }

  std::string magic;
  if (!std::getline(in, magic)) {
    throw std::runtime_error("Checkpoint is empty: " + path);
  }
  if (magic != kFormatMagic) {
    throw std::runtime_error("Unsupported checkpoint format: " + magic);
  }

  InfosetTable loaded;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    const auto parts = SplitTab(line);
    if (parts.size() != 4) {
      throw std::runtime_error("Malformed checkpoint row: " + line);
    }

    const std::string& key = parts[0];
    const std::size_t action_count = static_cast<std::size_t>(std::stoull(parts[1]));
    InfosetNode node;
    node.regret_sum = SplitDoubles(parts[2], action_count);
    node.strategy_sum = SplitDoubles(parts[3], action_count);
    loaded.Put(key, std::move(node));
  }

  table = std::move(loaded);
}

}  // namespace plarbius::cfr

