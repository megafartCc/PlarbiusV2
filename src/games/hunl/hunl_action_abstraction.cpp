#include "plarbius/games/hunl/hunl_action_abstraction.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace plarbius::games::hunl {

namespace {

constexpr double kEpsilon = 1e-9;
constexpr std::uint16_t kAllInActionId = std::numeric_limits<std::uint16_t>::max();

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

std::vector<double> ParseDoubleList(const std::string& text) {
  std::vector<double> values;
  std::stringstream input(text);
  std::string item;
  while (std::getline(input, item, ',')) {
    const std::string token = Trim(item);
    if (token.empty()) {
      continue;
    }
    values.push_back(std::stod(token));
  }
  return values;
}

std::string SerializeDoubleList(const std::vector<double>& values) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    out << values[i];
  }
  return out.str();
}

bool ParseBool(const std::string& text) {
  const std::string token = Trim(text);
  if (token == "true" || token == "1" || token == "yes") {
    return true;
  }
  if (token == "false" || token == "0" || token == "no") {
    return false;
  }
  throw std::invalid_argument("Invalid bool value: " + text);
}

std::unordered_map<std::string, std::string> ParseKeyValueFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("Failed to open config file: " + path);
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

std::vector<double> NormalizePositive(const std::vector<double>& input, const char* name) {
  if (input.empty()) {
    throw std::invalid_argument(std::string(name) + " must not be empty.");
  }
  std::vector<double> out;
  out.reserve(input.size());
  for (const double value : input) {
    if (!(value > 0.0)) {
      throw std::invalid_argument(std::string(name) + " contains non-positive values.");
    }
    out.push_back(value);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end(), [](double a, double b) {
              return std::fabs(a - b) < 1e-6;
            }),
            out.end());
  return out;
}

void PushUniqueBetAction(std::vector<game::Action>* actions,
                         std::uint16_t abstraction_id,
                         double amount) {
  for (const auto& existing : *actions) {
    if (existing.type != game::ActionType::kBet) {
      continue;
    }
    if (std::fabs(existing.amount - amount) < 1e-6) {
      return;
    }
  }
  actions->push_back(game::Action{game::ActionType::kBet, abstraction_id, amount});
}

std::string FormatAmount(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << value;
  return out.str();
}

}  // namespace

HunlActionAbstractionConfig DefaultHunlActionAbstractionConfig() {
  return HunlActionAbstractionConfig{};
}

HunlActionAbstractionConfig LoadHunlActionAbstractionConfig(const std::string& path) {
  HunlActionAbstractionConfig config = DefaultHunlActionAbstractionConfig();
  const auto values = ParseKeyValueFile(path);

  if (const auto it = values.find("preflop_raise_sizes_bb"); it != values.end()) {
    config.preflop_raise_sizes_bb = ParseDoubleList(it->second);
  }
  if (const auto it = values.find("postflop_bet_sizes_pot"); it != values.end()) {
    config.postflop_bet_sizes_pot = ParseDoubleList(it->second);
  }
  if (const auto it = values.find("postflop_raise_sizes_pot"); it != values.end()) {
    config.postflop_raise_sizes_pot = ParseDoubleList(it->second);
  }
  if (const auto it = values.find("max_raises_per_round"); it != values.end()) {
    config.max_raises_per_round = static_cast<std::size_t>(std::stoull(it->second));
  }
  if (const auto it = values.find("allow_all_in"); it != values.end()) {
    config.allow_all_in = ParseBool(it->second);
  }
  if (const auto it = values.find("min_bet_bb"); it != values.end()) {
    config.min_bet_bb = std::stod(it->second);
  }

  config.preflop_raise_sizes_bb =
      NormalizePositive(config.preflop_raise_sizes_bb, "preflop_raise_sizes_bb");
  config.postflop_bet_sizes_pot =
      NormalizePositive(config.postflop_bet_sizes_pot, "postflop_bet_sizes_pot");
  config.postflop_raise_sizes_pot =
      NormalizePositive(config.postflop_raise_sizes_pot, "postflop_raise_sizes_pot");

  if (config.min_bet_bb <= 0.0) {
    throw std::invalid_argument("min_bet_bb must be > 0.");
  }
  return config;
}

void SaveHunlActionAbstractionConfig(const HunlActionAbstractionConfig& config,
                                     const std::string& path) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("Failed to write action abstraction config: " + path);
  }
  out << "# HUNL action abstraction config\n";
  out << "preflop_raise_sizes_bb=" << SerializeDoubleList(config.preflop_raise_sizes_bb) << '\n';
  out << "postflop_bet_sizes_pot=" << SerializeDoubleList(config.postflop_bet_sizes_pot) << '\n';
  out << "postflop_raise_sizes_pot=" << SerializeDoubleList(config.postflop_raise_sizes_pot) << '\n';
  out << "max_raises_per_round=" << config.max_raises_per_round << '\n';
  out << "allow_all_in=" << (config.allow_all_in ? "true" : "false") << '\n';
  out << "min_bet_bb=" << config.min_bet_bb << '\n';
}

std::vector<game::Action> BuildHunlLegalActions(const HunlActionAbstractionConfig& config,
                                                const HunlLegalActionContext& context) {
  std::vector<game::Action> actions;
  const double stack = std::max(0.0, context.stack_to_act_bb);
  const double to_call = std::max(0.0, context.to_call_bb);
  const double pot = std::max(1.0, context.pot_bb);

  if (stack <= kEpsilon) {
    return actions;
  }

  std::uint16_t abstraction_id = 1;
  if (context.facing_bet) {
    const double call_amount = std::min(stack, to_call);
    actions.push_back(game::Action{game::ActionType::kCall, 0, call_amount});
    actions.push_back(game::Action{game::ActionType::kFold, 0, 0.0});
  } else {
    actions.push_back(game::Action{game::ActionType::kCheck, 0, 0.0});
  }

  if (context.raises_in_round >= config.max_raises_per_round) {
    return actions;
  }
  if (stack <= to_call + kEpsilon) {
    return actions;
  }

  if (context.street == 0) {
    for (const double raise_size : config.preflop_raise_sizes_bb) {
      double pay = to_call + std::max(config.min_bet_bb, raise_size);
      pay = std::min(pay, stack);
      if (pay <= to_call + kEpsilon) {
        continue;
      }
      PushUniqueBetAction(&actions, abstraction_id++, pay);
    }
  } else {
    const auto& fractions =
        context.facing_bet ? config.postflop_raise_sizes_pot : config.postflop_bet_sizes_pot;
    for (const double fraction : fractions) {
      double pay = to_call + std::max(config.min_bet_bb, fraction * pot);
      pay = std::min(pay, stack);
      if (pay <= to_call + kEpsilon) {
        continue;
      }
      PushUniqueBetAction(&actions, abstraction_id++, pay);
    }
  }

  if (config.allow_all_in && stack > to_call + kEpsilon) {
    PushUniqueBetAction(&actions, kAllInActionId, stack);
  }

  return actions;
}

std::string DescribeHunlAction(const game::Action& action) {
  if (action.type == game::ActionType::kCheck) {
    return "x";
  }
  if (action.type == game::ActionType::kCall) {
    return "c";
  }
  if (action.type == game::ActionType::kFold) {
    return "f";
  }
  if (action.type == game::ActionType::kBet) {
    if (action.abstraction_id == kAllInActionId) {
      return "ai";
    }
    return "b" + std::to_string(action.abstraction_id) + "@" + FormatAmount(action.amount);
  }
  return "u";
}

}  // namespace plarbius::games::hunl
