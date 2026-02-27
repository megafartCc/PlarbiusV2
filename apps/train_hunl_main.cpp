#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "plarbius/cfr/cfr_plus_trainer.hpp"
#include "plarbius/cfr/mccfr_trainer.hpp"
#include "plarbius/cfr/regret_matcher.hpp"
#include "plarbius/cfr/training_config.hpp"
#include "plarbius/cfr/training_metrics.hpp"
#include "plarbius/cfr/infoset_checkpoint_io.hpp"
#include "plarbius/games/hunl/hunl_action_abstraction.hpp"
#include "plarbius/games/hunl/hunl_bucket_config.hpp"
#include "plarbius/games/hunl/hunl_game.hpp"
#include "plarbius/games/hunl/hunl_stack_bucket_config.hpp"
#include "plarbius/policy/policy_io.hpp"
#include "plarbius/infra/ipc_table_server.hpp"

std::atomic<bool> g_quit{false};
void HandleSigInt(int) { g_quit = true; }

namespace {

struct CliOptions {
  enum class BucketMode {
    kEquity,
    kHash,
  };

  std::string algo = "mccfr";
  std::string metrics_out_path;
  std::string policy_out_path;
  std::string bucket_config_path;
  std::string action_config_path;
  std::string stack_bucket_config_path;
  BucketMode bucket_mode = BucketMode::kEquity;
  bool bucket_mode_explicit = false;
  bool print_strategy = false;
  std::size_t strategy_limit = 32;
  plarbius::cfr::TrainerConfig trainer_config;
  plarbius::games::hunl::HunlGameConfig game_config;

  std::string ipc_server_name;
  std::string ipc_worker_name;
  std::size_t ipc_capacity = 131072; // default to 128k ring buffer
};

class HunlMetricsCsvWriter {
 public:
  HunlMetricsCsvWriter(std::string path,
                       std::string algo,
                       std::uint64_t seed)
      : path_(std::move(path)), algo_(std::move(algo)), seed_(seed) {
    out_.open(path_, std::ios::trunc);
    if (!out_) {
      throw std::runtime_error("Failed to open metrics file: " + path_);
    }
    out_ << "algorithm,game,seed,iteration,infosets,utility_p0,utility_p1,best_response_p0,"
            "best_response_p1,nash_conv,exploitability\n";
  }

  void Write(std::uint64_t iteration, const plarbius::cfr::InfosetTable& table) {
    if (iteration == last_iteration_) {
      return;
    }
    out_ << algo_ << ",hunl," << seed_ << ',' << iteration << ',' << table.Size() << ",,,,,,\n";
    last_iteration_ = iteration;
    out_.flush();
  }

 private:
  std::string path_;
  std::string algo_;
  std::uint64_t seed_;
  std::ofstream out_;
  std::uint64_t last_iteration_ = 0;
};

[[nodiscard]] bool LooksLikeFlag(std::string_view value) {
  return value.rfind("--", 0) == 0;
}

[[nodiscard]] std::uint64_t ParseUnsigned(const std::string& text, const char* name) {
  try {
    return std::stoull(text);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string("Invalid value for ") + name + ": " + text);
  }
}

[[nodiscard]] double ParseDouble(const std::string& text, const char* name) {
  try {
    return std::stod(text);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string("Invalid value for ") + name + ": " + text);
  }
}

[[nodiscard]] std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

[[nodiscard]] const char* BucketModeLabel(CliOptions::BucketMode mode) {
  return mode == CliOptions::BucketMode::kHash ? "hash" : "equity";
}

[[nodiscard]] std::string ReadFileText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Failed to read file: " + path);
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string FormatDouble(double value) {
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return out.str();
}

[[nodiscard]] std::string JoinDoubles(const std::vector<double>& values) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    out << FormatDouble(values[i]);
  }
  return out.str();
}

[[nodiscard]] std::string SerializeActionConfigCanonical(
    const plarbius::games::hunl::HunlActionAbstractionConfig& config) {
  std::ostringstream out;
  out << "preflop_open_sizes_bb=" << JoinDoubles(config.preflop_open_sizes_bb) << '\n';
  out << "preflop_raise_sizes_bb=" << JoinDoubles(config.preflop_raise_sizes_bb) << '\n';
  out << "flop_bet_sizes_pot=" << JoinDoubles(config.flop_bet_sizes_pot) << '\n';
  out << "turn_bet_sizes_pot=" << JoinDoubles(config.turn_bet_sizes_pot) << '\n';
  out << "river_bet_sizes_pot=" << JoinDoubles(config.river_bet_sizes_pot) << '\n';
  out << "flop_raise_sizes_pot=" << JoinDoubles(config.flop_raise_sizes_pot) << '\n';
  out << "turn_raise_sizes_pot=" << JoinDoubles(config.turn_raise_sizes_pot) << '\n';
  out << "river_raise_sizes_pot=" << JoinDoubles(config.river_raise_sizes_pot) << '\n';
  out << "max_raises_per_round=" << config.max_raises_per_round << '\n';
  out << "allow_all_in=" << (config.allow_all_in ? "true" : "false") << '\n';
  out << "min_bet_bb=" << FormatDouble(config.min_bet_bb) << '\n';
  return out.str();
}

[[nodiscard]] std::string SerializeBucketConfigCanonical(
    const plarbius::games::hunl::HunlBucketConfig& config) {
  std::ostringstream out;
  out << "preflop_private_buckets=" << config.preflop_private_buckets << '\n';
  out << "flop_private_buckets=" << config.flop_private_buckets << '\n';
  out << "turn_private_buckets=" << config.turn_private_buckets << '\n';
  out << "river_private_buckets=" << config.river_private_buckets << '\n';
  out << "flop_public_buckets=" << config.flop_public_buckets << '\n';
  out << "turn_public_buckets=" << config.turn_public_buckets << '\n';
  out << "river_public_buckets=" << config.river_public_buckets << '\n';
  out << "chance_outcomes=" << config.chance_outcomes << '\n';
  out << "use_equity_buckets=" << (config.use_equity_buckets ? "true" : "false") << '\n';
  out << "equity_samples_per_outcome=" << config.equity_samples_per_outcome << '\n';
  out << "equity_bucket_seed=" << config.equity_bucket_seed << '\n';
  out << "infoset_pot_bucket_width_bb=" << FormatDouble(config.infoset_pot_bucket_width_bb) << '\n';
  out << "infoset_to_call_bucket_width_bb=" << FormatDouble(config.infoset_to_call_bucket_width_bb) << '\n';
  out << "infoset_stack_bucket_width_bb=" << FormatDouble(config.infoset_stack_bucket_width_bb) << '\n';
  out << "infoset_pot_bucket_cap=" << config.infoset_pot_bucket_cap << '\n';
  out << "infoset_to_call_bucket_cap=" << config.infoset_to_call_bucket_cap << '\n';
  out << "infoset_stack_bucket_cap=" << config.infoset_stack_bucket_cap << '\n';
  out << "preflop_strength_weight=" << FormatDouble(config.preflop_strength_weight) << '\n';
  out << "flop_strength_weight=" << FormatDouble(config.flop_strength_weight) << '\n';
  out << "turn_strength_weight=" << FormatDouble(config.turn_strength_weight) << '\n';
  out << "river_strength_weight=" << FormatDouble(config.river_strength_weight) << '\n';
  out << "private_bucket_mix=" << FormatDouble(config.private_bucket_mix) << '\n';
  return out.str();
}

[[nodiscard]] std::string SerializeStackBucketConfigCanonical(
    const plarbius::games::hunl::HunlStackBucketConfig& config) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"effective_stack_bb_edges\": [" << JoinDoubles(config.effective_stack_bb_edges) << "],\n";
  out << "  \"ante_bb_edges\": [" << JoinDoubles(config.ante_bb_edges) << "],\n";
  out << "  \"include_ante_bucket\": " << (config.include_ante_bucket ? "true" : "false") << '\n';
  out << "}\n";
  return out.str();
}

[[nodiscard]] std::uint64_t Fnv1a64(std::string_view text) {
  constexpr std::uint64_t kOffset = 14695981039346656037ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  std::uint64_t hash = kOffset;
  for (unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kPrime;
  }
  return hash;
}

[[nodiscard]] std::string Hex64(std::uint64_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kHex[value & 0xF];
    value >>= 4U;
  }
  return out;
}

[[nodiscard]] std::string TaggedHash(std::string_view text) {
  return std::string("fnv1a64:") + Hex64(Fnv1a64(text));
}

[[nodiscard]] std::string BuildSchemaId(const std::string& action_hash,
                                        const std::string& bucket_hash,
                                        const std::string& stack_hash,
                                        bool use_equity_buckets,
                                        CliOptions::BucketMode bucket_mode) {
  std::ostringstream out;
  out << "action_config_hash=" << action_hash << '\n';
  out << "bucket_config_hash=" << bucket_hash << '\n';
  out << "stack_bucket_config_hash=" << stack_hash << '\n';
  out << "use_equity_buckets=" << (use_equity_buckets ? "true" : "false") << '\n';
  out << "bucket_mode=" << BucketModeLabel(bucket_mode) << '\n';
  return TaggedHash(out.str());
}

void SaveSchemaJson(const std::string& path,
                    const std::string& schema_id,
                    const std::string& action_hash,
                    const std::string& bucket_hash,
                    const std::string& stack_hash,
                    bool use_equity_buckets,
                    CliOptions::BucketMode bucket_mode) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("Failed to write schema file: " + path);
  }
  out << "{\n";
  out << "  \"schema_version\": 1,\n";
  out << "  \"schema_id\": \"" << schema_id << "\",\n";
  out << "  \"bucket_mode\": \"" << BucketModeLabel(bucket_mode) << "\",\n";
  out << "  \"use_equity_buckets\": " << (use_equity_buckets ? "true" : "false") << ",\n";
  out << "  \"components\": {\n";
  out << "    \"action_config_hash\": \"" << action_hash << "\",\n";
  out << "    \"bucket_config_hash\": \"" << bucket_hash << "\",\n";
  out << "    \"stack_bucket_config_hash\": \"" << stack_hash << "\"\n";
  out << "  }\n";
  out << "}\n";
}

void ValidateBucketModeForBuild(CliOptions::BucketMode mode) {
  if (mode != CliOptions::BucketMode::kHash) {
    return;
  }
#if defined(NDEBUG)
  throw std::invalid_argument(
      "--bucket_mode=hash is debug-only and is blocked in release builds. "
      "Use --bucket_mode=equity.");
#else
  std::cerr << "\n"
            << "############################################################\n"
            << "### WARNING: HASH BUCKET MODE ENABLED (DEBUG-ONLY MODE) ###\n"
            << "### This mode is for diagnostics only and not for models. ###\n"
            << "### DO NOT deploy models trained with --bucket_mode=hash. ###\n"
            << "############################################################\n\n";
#endif
}

void PrintUsage() {
  std::cout << "Usage:\n"
            << "  plarbius_train_hunl [iterations] [--algo cfr+|mccfr] [--seed N]\n"
            << "                      [--bucket-config path] [--action-config path]\n"
            << "                      [--bucket_mode equity|hash]\n"
            << "                      [--stack-bucket-config path]\n"
            << "                      [--stack-bb X] [--small-blind-bb X] [--big-blind-bb X]\n"
            << "                      [--ante-bb X]\n"
            << "                      [--sampling-epsilon X]\n"
            << "                      [--lcfr-discount] [--lcfr-start N] [--lcfr-interval N]\n"
            << "                      [--lcfr-no-strategy-discount]\n"
            << "                      [--prune-actions] [--prune-start N] [--prune-threshold X]\n"
            << "                      [--prune-min-actions N] [--prune-full-interval N]\n"
            << "                      [--threads N] [--pin-physical-cores]\n"
            << "                      [--checkpoint path] [--checkpoint-every N] [--resume path]\n"
            << "                      [--log-interval N] [--avg-delay N]\n"
            << "                      [--metrics-out path] [--metrics-interval N]\n"
            << "                      [--policy-out path] [--strategy-limit N] [--no-strategy-print]\n";
}

void PrintHunlConfigSummary(const plarbius::games::hunl::HunlGameConfig& config) {
  std::cout << "HUNL abstraction config\n";
  std::cout << "stack_bb=" << config.stack_bb
            << " blinds=" << config.small_blind_bb << "/" << config.big_blind_bb << '\n';
  std::cout << "ante_bb=" << config.ante_bb << '\n';
  std::cout << "bucket_mode=" << (config.bucket_config.use_equity_buckets ? "equity" : "hash")
            << " use_equity_buckets="
            << (config.bucket_config.use_equity_buckets ? "true" : "false") << '\n';
  std::cout << "buckets private preflop/flop/turn/river="
            << config.bucket_config.preflop_private_buckets << '/'
            << config.bucket_config.flop_private_buckets << '/'
            << config.bucket_config.turn_private_buckets << '/'
            << config.bucket_config.river_private_buckets << '\n';
  std::cout << "buckets public flop/turn/river="
            << config.bucket_config.flop_public_buckets << '/'
            << config.bucket_config.turn_public_buckets << '/'
            << config.bucket_config.river_public_buckets
            << " chance_outcomes=" << config.bucket_config.chance_outcomes << '\n';
  std::cout << "action abstraction raises_per_round="
            << config.action_config.max_raises_per_round
            << " all_in=" << (config.action_config.allow_all_in ? "true" : "false")
            << " min_bet_bb=" << config.action_config.min_bet_bb << '\n';
  std::cout << "effective stack bucket edges bb=";
  for (std::size_t i = 0; i < config.stack_bucket_config.effective_stack_bb_edges.size(); ++i) {
    if (i > 0) {
      std::cout << ',';
    }
    std::cout << config.stack_bucket_config.effective_stack_bb_edges[i];
  }
  std::cout << '\n';
  std::cout << "include_ante_bucket=" << (config.stack_bucket_config.include_ante_bucket ? "true" : "false")
            << '\n';
}

void PrintStrategySnapshot(const plarbius::cfr::InfosetTable& table, std::size_t strategy_limit) {
  std::vector<std::pair<std::string, plarbius::cfr::InfosetNode>> snapshot;
  snapshot.reserve(table.Size());
  for (const auto& entry : table.Nodes()) {
    snapshot.emplace_back(entry.key, entry.node);
  }
  std::sort(snapshot.begin(), snapshot.end(), [](const auto& left, const auto& right) {
    return left.first < right.first;
  });

  std::cout << "\nAverage strategy snapshot\n";
  std::cout << "infosets=" << snapshot.size() << '\n';
  const std::size_t limit = std::min(strategy_limit, snapshot.size());
  for (std::size_t row = 0; row < limit; ++row) {
    const auto& key = snapshot[row].first;
    const auto& node = snapshot[row].second;
    const auto avg = plarbius::cfr::RegretMatcher::Normalize(node.strategy_sum);
    std::cout << key;
    for (double p : avg) {
      std::cout << ' ' << std::fixed << std::setprecision(3) << p;
    }
    std::cout << '\n';
  }
  if (snapshot.size() > limit) {
    std::cout << "... truncated " << (snapshot.size() - limit) << " infosets\n";
  }
}

[[nodiscard]] int ExtractStreetFromInfosetKey(const std::string& key) {
  const std::size_t s_pos = key.find("|s");
  if (s_pos == std::string::npos || (s_pos + 2) >= key.size()) {
    return -1;
  }
  const char ch = key[s_pos + 2];
  if (ch < '0' || ch > '9') {
    return -1;
  }
  return static_cast<int>(ch - '0');
}

[[nodiscard]] double QuantileFromSorted(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double clamped_q = std::clamp(q, 0.0, 1.0);
  const double idx = clamped_q * static_cast<double>(sorted.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(idx);
  const std::size_t hi = std::min<std::size_t>(sorted.size() - 1, lo + 1);
  const double frac = idx - static_cast<double>(lo);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

[[nodiscard]] bool TryParseBucketTag(const std::string& key,
                                     const std::string& marker,
                                     std::uint32_t* value_out) {
  if (value_out == nullptr) {
    return false;
  }
  const std::size_t marker_pos = key.find(marker);
  if (marker_pos == std::string::npos) {
    return false;
  }
  const std::size_t start = marker_pos + marker.size();
  std::size_t end = start;
  while (end < key.size() && key[end] >= '0' && key[end] <= '9') {
    ++end;
  }
  if (end <= start) {
    return false;
  }
  *value_out = static_cast<std::uint32_t>(
      std::strtoul(key.substr(start, end - start).c_str(), nullptr, 10));
  return true;
}

[[nodiscard]] std::string ExtractHistorySuffix(const std::string& key) {
  const std::size_t pos = key.rfind('|');
  if (pos == std::string::npos || (pos + 1) >= key.size()) {
    return std::string{};
  }
  return key.substr(pos + 1);
}

void PrintInfosetStats(const plarbius::cfr::InfosetTable& table) {
  std::vector<double> visits;
  visits.reserve(table.Size());
  std::array<std::vector<double>, 4> street_visits{};

  for (const auto& entry : table.Nodes()) {
    const double count = static_cast<double>(
        entry.node.visit_count.load(std::memory_order_relaxed));
    visits.push_back(count);
    const int street = ExtractStreetFromInfosetKey(entry.key);
    if (street >= 0 && street < 4) {
      street_visits[static_cast<std::size_t>(street)].push_back(count);
    }
  }

  std::sort(visits.begin(), visits.end());
  std::size_t lt10 = 0;
  for (double value : visits) {
    if (value < 10.0) {
      lt10 += 1;
    }
  }
  const double pct_lt10 = visits.empty()
                              ? 0.0
                              : (100.0 * static_cast<double>(lt10) / static_cast<double>(visits.size()));

  std::cout << "[INFOSET_STATS]\n";
  std::cout << "total=" << visits.size() << '\n';
  std::cout << "p50=" << QuantileFromSorted(visits, 0.50) << '\n';
  std::cout << "p90=" << QuantileFromSorted(visits, 0.90) << '\n';
  std::cout << "p99=" << QuantileFromSorted(visits, 0.99) << '\n';
  std::cout << "pct_lt10=" << pct_lt10 << '\n';

  static constexpr std::array<const char*, 4> kStreetNames = {"preflop", "flop", "turn", "river"};
  for (std::size_t street = 0; street < street_visits.size(); ++street) {
    auto values = street_visits[street];
    std::sort(values.begin(), values.end());
    std::size_t street_lt10 = 0;
    for (double value : values) {
      if (value < 10.0) {
        street_lt10 += 1;
      }
    }
    const double street_pct_lt10 = values.empty()
                                       ? 0.0
                                       : (100.0 * static_cast<double>(street_lt10) /
                                          static_cast<double>(values.size()));
    std::cout << "[INFOSET_STATS] street=" << kStreetNames[street]
              << " total=" << values.size()
              << " p50=" << QuantileFromSorted(values, 0.50)
              << " p90=" << QuantileFromSorted(values, 0.90)
              << " p99=" << QuantileFromSorted(values, 0.99)
              << " pct_lt10=" << street_pct_lt10
              << '\n';
  }
}

void PrintInfosetBucketHistograms(const plarbius::cfr::InfosetTable& table) {
  using BucketHist = std::map<std::uint32_t, std::uint64_t>;
  std::array<BucketHist, 4> spr_hist{};
  std::array<BucketHist, 4> facing_hist{};
  std::array<BucketHist, 4> pot_hist{};
  std::array<BucketHist, 4> to_call_hist{};
  std::array<BucketHist, 4> stack_hist{};
  std::array<BucketHist, 4> eff_hist{};

  for (const auto& entry : table.Nodes()) {
    const int street = ExtractStreetFromInfosetKey(entry.key);
    if (street < 0 || street >= 4) {
      continue;
    }
    const std::size_t idx = static_cast<std::size_t>(street);
    std::uint32_t value = 0;
    if (TryParseBucketTag(entry.key, "|spr", &value)) {
      spr_hist[idx][value] += 1;
    }
    if (TryParseBucketTag(entry.key, "|fb", &value)) {
      facing_hist[idx][value] += 1;
    }
    if (TryParseBucketTag(entry.key, "|pot", &value)) {
      pot_hist[idx][value] += 1;
    }
    if (TryParseBucketTag(entry.key, "|tc", &value)) {
      to_call_hist[idx][value] += 1;
    }
    if (TryParseBucketTag(entry.key, "|stk", &value)) {
      stack_hist[idx][value] += 1;
    }
    if (TryParseBucketTag(entry.key, "|eff", &value)) {
      eff_hist[idx][value] += 1;
    }
  }

  static constexpr std::array<const char*, 4> kStreetNames = {"preflop", "flop", "turn", "river"};
  const auto print_hist_field = [&](const char* field,
                                    const std::array<BucketHist, 4>& histograms) {
    for (std::size_t street = 0; street < histograms.size(); ++street) {
      const auto& hist = histograms[street];
      std::uint64_t total = 0;
      for (const auto& [bucket, count] : hist) {
        (void)bucket;
        total += count;
      }
      if (total == 0) {
        continue;
      }

      std::vector<std::pair<std::uint32_t, std::uint64_t>> ordered(hist.begin(), hist.end());
      std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) {
          return left.second > right.second;
        }
        return left.first < right.first;
      });

      std::cout << "[INFOSET_BUCKET_HIST] street=" << kStreetNames[street]
                << " field=" << field
                << " total=" << total
                << " distinct=" << hist.size()
                << " top=";
      const std::size_t top_n = std::min<std::size_t>(5, ordered.size());
      for (std::size_t i = 0; i < top_n; ++i) {
        const double share = (100.0 * static_cast<double>(ordered[i].second)) /
                             static_cast<double>(total);
        if (i > 0) {
          std::cout << ',';
        }
        std::cout << ordered[i].first << ':' << ordered[i].second
                  << '(' << std::fixed << std::setprecision(2) << share << "%)";
      }
      std::cout << '\n';
    }
  };

  print_hist_field("spr", spr_hist);
  print_hist_field("fb", facing_hist);
  print_hist_field("pot", pot_hist);
  print_hist_field("tc", to_call_hist);
  print_hist_field("stk", stack_hist);
  print_hist_field("eff", eff_hist);
}

void PrintInfosetExplosionReport(const plarbius::cfr::InfosetTable& table,
                                 std::uint64_t iterations) {
  static constexpr std::array<const char*, 4> kStreetNames = {"preflop", "flop", "turn", "river"};
  constexpr std::size_t kPrefixLen = 24;

  using CountMap = std::map<std::string, std::uint64_t>;
  using LengthMap = std::map<std::size_t, std::uint64_t>;
  std::array<CountMap, 4> history_prefixes{};
  std::array<LengthMap, 4> history_lengths{};
  std::array<CountMap, 4> combo_counts{};
  std::array<std::uint64_t, 4> street_infosets{};

  for (const auto& entry : table.Nodes()) {
    const int street = ExtractStreetFromInfosetKey(entry.key);
    if (street < 0 || street >= 4) {
      continue;
    }
    const std::size_t idx = static_cast<std::size_t>(street);
    street_infosets[idx] += 1;

    const std::string history_suffix = ExtractHistorySuffix(entry.key);
    history_lengths[idx][history_suffix.size()] += 1;
    const std::string prefix =
        history_suffix.size() <= kPrefixLen ? history_suffix : history_suffix.substr(0, kPrefixLen);
    history_prefixes[idx][prefix] += 1;

    std::uint32_t spr = 0;
    std::uint32_t fb = 0;
    std::uint32_t pr = 0;
    std::uint32_t pb = 0;
    std::uint32_t raises = 0;
    if (TryParseBucketTag(entry.key, "|spr", &spr) &&
        TryParseBucketTag(entry.key, "|fb", &fb) &&
        TryParseBucketTag(entry.key, "|pr", &pr) &&
        TryParseBucketTag(entry.key, "|pb", &pb) &&
        TryParseBucketTag(entry.key, "|r", &raises)) {
      std::ostringstream combo;
      combo << "spr=" << spr
            << ",fb=" << fb
            << ",pr=" << pr
            << ",pb=" << pb
            << ",r=" << raises;
      combo_counts[idx][combo.str()] += 1;
    }
  }

  std::vector<std::pair<std::string, std::uint64_t>> street_contributors;
  street_contributors.reserve(4);

  for (std::size_t street = 0; street < 4; ++street) {
    const std::uint64_t total = street_infosets[street];
    const double per_100k = iterations == 0
                                ? 0.0
                                : (100000.0 * static_cast<double>(total) /
                                   static_cast<double>(iterations));
    street_contributors.emplace_back(kStreetNames[street], total);

    std::cout << "[INFOSET_EXPLOSION] street=" << kStreetNames[street]
              << " infosets=" << total
              << " new_per_100k_iters=" << per_100k
              << " unique_history_lengths=" << history_lengths[street].size()
              << " unique_spr_fb_pr_pb_r=" << combo_counts[street].size()
              << '\n';

    auto print_top = [&](const CountMap& counts, const char* label) {
      if (counts.empty()) {
        return;
      }
      std::vector<std::pair<std::string, std::uint64_t>> ordered(counts.begin(), counts.end());
      std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.second != right.second) {
          return left.second > right.second;
        }
        return left.first < right.first;
      });

      std::cout << "[INFOSET_EXPLOSION] street=" << kStreetNames[street] << ' ' << label;
      const std::size_t top_n = std::min<std::size_t>(5, ordered.size());
      for (std::size_t i = 0; i < top_n; ++i) {
        if (i > 0) {
          std::cout << ',';
        }
        std::cout << ordered[i].first << ':' << ordered[i].second;
      }
      std::cout << '\n';
    };

    print_top(history_prefixes[street], "top_history_prefixes=");
    print_top(combo_counts[street], "top_spr_fb_pr_pb_r=");
  }

  std::sort(street_contributors.begin(), street_contributors.end(), [](const auto& left, const auto& right) {
    if (left.second != right.second) {
      return left.second > right.second;
    }
    return left.first < right.first;
  });
  std::cout << "[INFOSET_EXPLOSION] top_street_contributors=";
  for (std::size_t i = 0; i < street_contributors.size(); ++i) {
    if (i > 0) {
      std::cout << ',';
    }
    std::cout << street_contributors[i].first << ':' << street_contributors[i].second;
  }
  std::cout << '\n';
}

void PrintSprRawTelemetry() {
  const auto snapshot = plarbius::games::hunl::SnapshotHunlSprRawTelemetry();
  static constexpr std::array<const char*, 4> kStreetNames = {"preflop", "flop", "turn", "river"};

  const auto approx_quantile_from_hist = [&](const std::array<std::uint64_t,
                                                               plarbius::games::hunl::kHunlSprRawBinCount>& hist,
                                             std::uint64_t total,
                                             double q) -> double {
    if (total == 0) {
      return 0.0;
    }
    const double target = std::clamp(q, 0.0, 1.0) * static_cast<double>(total);
    double cumulative = 0.0;
    for (std::size_t i = 0; i < hist.size(); ++i) {
      cumulative += static_cast<double>(hist[i]);
      if (cumulative >= target) {
        if (i < snapshot.bin_edges.size()) {
          return snapshot.bin_edges[i];
        }
        return snapshot.bin_edges.back();
      }
    }
    return snapshot.bin_edges.back();
  };

  for (std::size_t street = 0; street < snapshot.streets.size(); ++street) {
    const auto& row = snapshot.streets[street];
    if (row.samples == 0) {
      continue;
    }
    const double p5 = approx_quantile_from_hist(row.histogram, row.samples, 0.05);
    const double p25 = approx_quantile_from_hist(row.histogram, row.samples, 0.25);
    const double p50 = approx_quantile_from_hist(row.histogram, row.samples, 0.50);
    const double p75 = approx_quantile_from_hist(row.histogram, row.samples, 0.75);
    const double p95 = approx_quantile_from_hist(row.histogram, row.samples, 0.95);

    std::cout << "[SPR_RAW_STATS] street=" << kStreetNames[street]
              << " samples=" << row.samples
              << " p5=" << p5
              << " p25=" << p25
              << " p50=" << p50
              << " p75=" << p75
              << " p95=" << p95
              << " hist=";

    bool first = true;
    for (std::size_t i = 0; i < row.histogram.size(); ++i) {
      const std::uint64_t count = row.histogram[i];
      if (count == 0) {
        continue;
      }
      const double pct = 100.0 * static_cast<double>(count) / static_cast<double>(row.samples);
      if (!first) {
        std::cout << ',';
      }
      first = false;
      if (i < snapshot.bin_edges.size()) {
        std::cout << "<=" << snapshot.bin_edges[i] << ':' << count
                  << '(' << std::fixed << std::setprecision(2) << pct << "%)";
      } else {
        std::cout << ">" << snapshot.bin_edges.back() << ':' << count
                  << '(' << std::fixed << std::setprecision(2) << pct << "%)";
      }
    }
    std::cout << '\n';
  }
}

CliOptions ParseCli(int argc, char** argv) {
  CliOptions options;
  options.trainer_config.iterations = 50000;

  int index = 1;
  if (argc > 1 && !LooksLikeFlag(argv[1])) {
    options.trainer_config.iterations = ParseUnsigned(argv[1], "iterations");
    index = 2;
  }

  while (index < argc) {
    const std::string flag = argv[index++];
    if (flag == "--algo") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --algo.");
      }
      options.algo = argv[index++];
    } else if (flag == "--seed") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --seed.");
      }
      options.trainer_config.seed = ParseUnsigned(argv[index++], "--seed");
    } else if (flag == "--bucket-config") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --bucket-config.");
      }
      options.bucket_config_path = argv[index++];
    } else if (flag == "--bucket_mode" || flag == "--bucket-mode") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --bucket_mode.");
      }
      const std::string mode = ToLower(argv[index++]);
      if (mode == "equity") {
        options.bucket_mode = CliOptions::BucketMode::kEquity;
      } else if (mode == "hash") {
        options.bucket_mode = CliOptions::BucketMode::kHash;
      } else {
        throw std::invalid_argument("Invalid --bucket_mode. Expected equity|hash.");
      }
      options.bucket_mode_explicit = true;
    } else if (flag == "--action-config") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --action-config.");
      }
      options.action_config_path = argv[index++];
    } else if (flag == "--stack-bucket-config") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --stack-bucket-config.");
      }
      options.stack_bucket_config_path = argv[index++];
    } else if (flag == "--stack-bb") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --stack-bb.");
      }
      options.game_config.stack_bb = ParseDouble(argv[index++], "--stack-bb");
    } else if (flag == "--small-blind-bb") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --small-blind-bb.");
      }
      options.game_config.small_blind_bb = ParseDouble(argv[index++], "--small-blind-bb");
    } else if (flag == "--big-blind-bb") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --big-blind-bb.");
      }
      options.game_config.big_blind_bb = ParseDouble(argv[index++], "--big-blind-bb");
    } else if (flag == "--ante-bb") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --ante-bb.");
      }
      options.game_config.ante_bb = ParseDouble(argv[index++], "--ante-bb");
    } else if (flag == "--sampling-epsilon") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --sampling-epsilon.");
      }
      options.trainer_config.sampling_epsilon = ParseDouble(argv[index++], "--sampling-epsilon");
    } else if (flag == "--lcfr-discount") {
      options.trainer_config.mccfr_use_lcfr_discount = true;
    } else if (flag == "--lcfr-start") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --lcfr-start.");
      }
      options.trainer_config.mccfr_lcfr_discount_start = ParseUnsigned(argv[index++], "--lcfr-start");
    } else if (flag == "--lcfr-interval") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --lcfr-interval.");
      }
      options.trainer_config.mccfr_lcfr_discount_interval =
          ParseUnsigned(argv[index++], "--lcfr-interval");
    } else if (flag == "--lcfr-no-strategy-discount") {
      options.trainer_config.mccfr_lcfr_discount_strategy_sum = false;
    } else if (flag == "--prune-actions") {
      options.trainer_config.mccfr_enable_pruning = true;
    } else if (flag == "--prune-start") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --prune-start.");
      }
      options.trainer_config.mccfr_prune_start = ParseUnsigned(argv[index++], "--prune-start");
    } else if (flag == "--prune-threshold") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --prune-threshold.");
      }
      options.trainer_config.mccfr_prune_regret_threshold =
          ParseDouble(argv[index++], "--prune-threshold");
    } else if (flag == "--prune-min-actions") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --prune-min-actions.");
      }
      options.trainer_config.mccfr_prune_min_actions =
          static_cast<std::size_t>(ParseUnsigned(argv[index++], "--prune-min-actions"));
    } else if (flag == "--prune-full-interval") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --prune-full-interval.");
      }
      options.trainer_config.mccfr_prune_full_traversal_interval =
          ParseUnsigned(argv[index++], "--prune-full-interval");
    } else if (flag == "--checkpoint") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --checkpoint.");
      }
      options.trainer_config.checkpoint_path = argv[index++];
    } else if (flag == "--ipc-server") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --ipc-server.");
      }
      options.ipc_server_name = argv[index++];
    } else if (flag == "--ipc-worker") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --ipc-worker.");
      }
      options.ipc_worker_name = argv[index++];
    } else if (flag == "--ipc-capacity") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --ipc-capacity.");
      }
      options.ipc_capacity = static_cast<std::size_t>(ParseUnsigned(argv[index++], "--ipc-capacity"));
    } else if (flag == "--checkpoint-every") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --checkpoint-every.");
      }
      options.trainer_config.checkpoint_every = ParseUnsigned(argv[index++], "--checkpoint-every");
    } else if (flag == "--resume") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --resume.");
      }
      options.trainer_config.resume_path = argv[index++];
    } else if (flag == "--log-interval") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --log-interval.");
      }
      options.trainer_config.log_interval = ParseUnsigned(argv[index++], "--log-interval");
    } else if (flag == "--avg-delay") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --avg-delay.");
      }
      options.trainer_config.averaging_delay = ParseUnsigned(argv[index++], "--avg-delay");
    } else if (flag == "--metrics-out") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --metrics-out.");
      }
      options.metrics_out_path = argv[index++];
    } else if (flag == "--metrics-interval") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --metrics-interval.");
      }
      options.trainer_config.metrics_interval = ParseUnsigned(argv[index++], "--metrics-interval");
    } else if (flag == "--threads") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --threads.");
      }
      options.trainer_config.num_threads =
          static_cast<std::size_t>(ParseUnsigned(argv[index++], "--threads"));
    } else if (flag == "--pin-physical-cores") {
      options.trainer_config.pin_physical_cores_only = true;
    } else if (flag == "--policy-out") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --policy-out.");
      }
      options.policy_out_path = argv[index++];
    } else if (flag == "--strategy-limit") {
      if (index >= argc) {
        throw std::invalid_argument("Missing value for --strategy-limit.");
      }
      options.strategy_limit = static_cast<std::size_t>(ParseUnsigned(argv[index++], "--strategy-limit"));
    } else if (flag == "--no-strategy-print") {
      options.print_strategy = false;
    } else if (flag == "--strategy-print") {
      options.print_strategy = true;
    } else if (flag == "--help" || flag == "-h") {
      PrintUsage();
      std::exit(0);
    } else {
      throw std::invalid_argument("Unknown flag: " + flag);
    }
  }

  if (!options.bucket_config_path.empty()) {
    options.game_config.bucket_config =
        plarbius::games::hunl::LoadHunlBucketConfig(options.bucket_config_path);
  }
  if (!options.action_config_path.empty()) {
    options.game_config.action_config =
        plarbius::games::hunl::LoadHunlActionAbstractionConfig(options.action_config_path);
  }
  if (!options.stack_bucket_config_path.empty()) {
    options.game_config.stack_bucket_config =
        plarbius::games::hunl::LoadHunlStackBucketConfig(options.stack_bucket_config_path);
  }
  options.game_config.bucket_config.use_equity_buckets =
      options.bucket_mode == CliOptions::BucketMode::kEquity;

  if (options.trainer_config.iterations == 0) {
    throw std::invalid_argument("iterations must be > 0.");
  }
  if (options.trainer_config.log_interval == 0) {
    options.trainer_config.log_interval =
        std::max<std::uint64_t>(1, options.trainer_config.iterations / 20);
  }
  if (options.trainer_config.averaging_delay == 0) {
    options.trainer_config.averaging_delay = options.trainer_config.iterations / 20;
  }
  if (options.trainer_config.metrics_interval == 0 && !options.metrics_out_path.empty()) {
    options.trainer_config.metrics_interval =
        options.trainer_config.checkpoint_every > 0
            ? options.trainer_config.checkpoint_every
            : options.trainer_config.log_interval;
  }
  if (options.trainer_config.mccfr_lcfr_discount_interval == 0) {
    options.trainer_config.mccfr_lcfr_discount_interval = 1;
  }
  if (options.trainer_config.mccfr_prune_min_actions == 0) {
    options.trainer_config.mccfr_prune_min_actions = 1;
  }

  return options;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSigInt);

  CliOptions options;
  try {
    options = ParseCli(argc, argv);
    ValidateBucketModeForBuild(options.bucket_mode);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    PrintUsage();
    return 1;
  }

  try {
    plarbius::games::hunl::ValidateHunlGameConfig(options.game_config);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }

  PrintHunlConfigSummary(options.game_config);

  if (options.algo == "cfr+" && options.game_config.bucket_config.chance_outcomes > 16) {
    std::cerr << "cfr+ on this HUNL scaffold is too expensive with chance_outcomes="
              << options.game_config.bucket_config.chance_outcomes
              << ". Use --algo mccfr or reduce chance_outcomes to <=16 in bucket config.\n";
    return 1;
  }

  plarbius::games::hunl::HunlGame game(options.game_config);
  plarbius::games::hunl::ResetHunlSprRawTelemetry();

  std::shared_ptr<HunlMetricsCsvWriter> metrics_writer;
  plarbius::cfr::TrainingMetricsCallback metrics_callback = nullptr;
  if (!options.metrics_out_path.empty()) {
    metrics_writer = std::make_shared<HunlMetricsCsvWriter>(
        options.metrics_out_path, options.algo, options.trainer_config.seed);
    metrics_callback = [metrics_writer](std::uint64_t iteration,
                                        const plarbius::cfr::InfosetTable& table) {
      metrics_writer->Write(iteration, table);
    };
  }

  plarbius::cfr::InfosetTable result_table;
  
  if (!options.ipc_server_name.empty()) {
    plarbius::infra::IpcTableServer server(options.ipc_server_name, options.ipc_capacity);
    plarbius::cfr::InfosetTable table;
    
    if (!options.trainer_config.resume_path.empty()) {
      plarbius::cfr::InfosetCheckpointIo::Load(table, options.trainer_config.resume_path);
    }
    
    std::cout << "Starting IPC server '" << options.ipc_server_name << "' with capacity " << options.ipc_capacity << "...\n";
    std::cout << "Waiting for workers to send updates. Press Ctrl+C to terminate and save policy.\n";
    
    auto last_log = std::chrono::steady_clock::now();
    
    while (!g_quit) {
      std::size_t processed = server.ProcessPending(table);
      
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 10) {
        std::cout << "IPC Server (" << options.ipc_server_name << "): " << table.Size() << " infosets tracked.\n";
        last_log = now;
      }
      
      if (std::filesystem::exists(".stop_ipc_server")) {
        std::cout << "\nReceived shutdown signal via .stop_ipc_server file.\n";
        g_quit = true;
        std::error_code ec;
        std::filesystem::remove(".stop_ipc_server", ec);
      }

      if (processed == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
      }
    }
    std::cout << "\nTerminating IPC server session.\n";
    result_table = std::move(table);
  } else {
    if (!options.ipc_worker_name.empty()) {
      options.trainer_config.ipc_client = std::make_shared<plarbius::infra::IpcTableClient>(options.ipc_worker_name);
      std::cout << "Attached worker to IPC server '" << options.ipc_worker_name << "'\n";
    }

    if (options.algo == "cfr+") {
      plarbius::cfr::CfrPlusTrainer trainer(game, options.trainer_config, nullptr, metrics_callback);
      trainer.Train();
      result_table = trainer.Table();
    } else if (options.algo == "mccfr") {
      plarbius::cfr::MccfrTrainer trainer(game, options.trainer_config, nullptr, metrics_callback);
      trainer.Train();
      result_table = trainer.Table();
    } else {
      std::cerr << "Unsupported algorithm: " << options.algo << '\n';
      return 1;
    }
  }

  if (options.print_strategy) {
    PrintStrategySnapshot(result_table, options.strategy_limit);
  }

  std::cout << "\nChecking policy save destination: " << (options.policy_out_path.empty() ? "[EMPTY]" : options.policy_out_path) << "\n";
  if (!options.policy_out_path.empty()) {
    std::cout << "Building average policy from " << result_table.Size() << " tracked infosets...\n";
    auto avg_policy = plarbius::policy::BuildAveragePolicy(result_table);
    std::cout << "Saving massive policy to " << options.policy_out_path << "...\n";
    plarbius::policy::SavePolicy(avg_policy, options.policy_out_path);
    std::cout << "policy_out=" << options.policy_out_path << " [SUCCESS]\n";

    const std::string action_text = options.action_config_path.empty()
                                        ? SerializeActionConfigCanonical(options.game_config.action_config)
                                        : ReadFileText(options.action_config_path);
    const std::string bucket_text = options.bucket_config_path.empty()
                                        ? SerializeBucketConfigCanonical(options.game_config.bucket_config)
                                        : ReadFileText(options.bucket_config_path);
    const std::string stack_text = options.stack_bucket_config_path.empty()
                                       ? SerializeStackBucketConfigCanonical(options.game_config.stack_bucket_config)
                                       : ReadFileText(options.stack_bucket_config_path);

    const std::string action_hash = TaggedHash(action_text);
    const std::string bucket_hash = TaggedHash(bucket_text);
    const std::string stack_hash = TaggedHash(stack_text);
    const std::string schema_id = BuildSchemaId(
        action_hash,
        bucket_hash,
        stack_hash,
        options.game_config.bucket_config.use_equity_buckets,
        options.bucket_mode);

    std::filesystem::path schema_path = std::filesystem::path(options.policy_out_path).parent_path();
    if (schema_path.empty()) {
      schema_path = std::filesystem::current_path();
    }
    schema_path /= "schema.json";
    SaveSchemaJson(
        schema_path.string(),
        schema_id,
        action_hash,
        bucket_hash,
        stack_hash,
        options.game_config.bucket_config.use_equity_buckets,
        options.bucket_mode);
    std::cout << "schema_out=" << schema_path.string() << '\n';
    std::cout << "schema_id=" << schema_id << '\n';
  }

  std::cout << "\nHUNL scaffold training complete.\n";
  std::cout << "infosets=" << result_table.Size() << '\n';
  PrintSprRawTelemetry();
  PrintInfosetStats(result_table);
  PrintInfosetBucketHistograms(result_table);
  PrintInfosetExplosionReport(result_table, options.trainer_config.iterations);
  std::cout << "note=exploitability evaluator is not yet implemented for hunl abstraction.\n";
  return 0;
}
