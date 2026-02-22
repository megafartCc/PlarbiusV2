#include "plarbius/games/hunl/hunl_bucket_config.hpp"

#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace plarbius::games::hunl {

namespace {

std::string Trim(std::string value) {
  const auto is_space = [](unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
  };
  while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::unordered_map<std::string, std::string> ParseKeyValueFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("Failed to open bucket config: " + path);
  }

  std::unordered_map<std::string, std::string> values;
  std::string line;
  while (std::getline(in, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    const std::size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string key = Trim(trimmed.substr(0, eq));
    const std::string value = Trim(trimmed.substr(eq + 1));
    if (!key.empty()) {
      values[key] = value;
    }
  }
  return values;
}

std::uint32_t ParseU32(const std::unordered_map<std::string, std::string>& values,
                       const char* key,
                       std::uint32_t fallback) {
  const auto it = values.find(key);
  if (it == values.end()) {
    return fallback;
  }
  return static_cast<std::uint32_t>(std::stoul(it->second));
}

void Validate(const HunlBucketConfig& config) {
  const auto is_valid = [](std::uint32_t value) {
    return value > 0;
  };
  if (!is_valid(config.preflop_private_buckets) ||
      !is_valid(config.flop_private_buckets) ||
      !is_valid(config.turn_private_buckets) ||
      !is_valid(config.river_private_buckets) ||
      !is_valid(config.flop_public_buckets) ||
      !is_valid(config.turn_public_buckets) ||
      !is_valid(config.river_public_buckets) ||
      !is_valid(config.chance_outcomes)) {
    throw std::invalid_argument("All HUNL bucket counts must be > 0.");
  }
}

}  // namespace

HunlBucketConfig DefaultHunlBucketConfig() {
  return HunlBucketConfig{};
}

HunlBucketConfig LoadHunlBucketConfig(const std::string& path) {
  HunlBucketConfig config = DefaultHunlBucketConfig();
  const auto values = ParseKeyValueFile(path);
  config.preflop_private_buckets =
      ParseU32(values, "preflop_private_buckets", config.preflop_private_buckets);
  config.flop_private_buckets = ParseU32(values, "flop_private_buckets", config.flop_private_buckets);
  config.turn_private_buckets = ParseU32(values, "turn_private_buckets", config.turn_private_buckets);
  config.river_private_buckets =
      ParseU32(values, "river_private_buckets", config.river_private_buckets);
  config.flop_public_buckets = ParseU32(values, "flop_public_buckets", config.flop_public_buckets);
  config.turn_public_buckets = ParseU32(values, "turn_public_buckets", config.turn_public_buckets);
  config.river_public_buckets =
      ParseU32(values, "river_public_buckets", config.river_public_buckets);
  config.chance_outcomes = ParseU32(values, "chance_outcomes", config.chance_outcomes);
  Validate(config);
  return config;
}

void SaveHunlBucketConfig(const HunlBucketConfig& config, const std::string& path) {
  Validate(config);
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("Failed to write bucket config: " + path);
  }
  out << "# HUNL bucket abstraction config\n";
  out << "preflop_private_buckets=" << config.preflop_private_buckets << '\n';
  out << "flop_private_buckets=" << config.flop_private_buckets << '\n';
  out << "turn_private_buckets=" << config.turn_private_buckets << '\n';
  out << "river_private_buckets=" << config.river_private_buckets << '\n';
  out << "flop_public_buckets=" << config.flop_public_buckets << '\n';
  out << "turn_public_buckets=" << config.turn_public_buckets << '\n';
  out << "river_public_buckets=" << config.river_public_buckets << '\n';
  out << "chance_outcomes=" << config.chance_outcomes << '\n';
}

}  // namespace plarbius::games::hunl
