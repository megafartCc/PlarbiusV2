#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "plarbius/cfr/infoset_table.hpp"
#include "plarbius/policy/policy_io.hpp"

namespace {

bool NearlyEqual(double a, double b, double epsilon = 1e-12) {
  return std::fabs(a - b) <= epsilon;
}

}  // namespace

int main() {
  plarbius::cfr::InfosetTable table;
  table.Put("0|J|_", plarbius::cfr::InfosetNode{
                        std::vector<double>{0.0, 0.0},
                        std::vector<double>{2.0, 8.0},
                    });
  table.Put("1|Q|b", plarbius::cfr::InfosetNode{
                        std::vector<double>{0.0, 1.0},
                        std::vector<double>{5.0, 5.0},
                    });

  const auto policy = plarbius::policy::BuildAveragePolicy(table);
  if (policy.size() != 2) {
    return 1;
  }

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "plarbius_policy_io_test.tsv";
  plarbius::policy::SavePolicy(policy, path.string());

  const auto loaded = plarbius::policy::LoadPolicy(path.string());
  std::filesystem::remove(path);

  if (loaded.size() != policy.size()) {
    return 1;
  }

  for (const auto& [key, probs] : policy) {
    const auto it = loaded.find(key);
    if (it == loaded.end() || it->second.size() != probs.size()) {
      return 1;
    }
    for (std::size_t i = 0; i < probs.size(); ++i) {
      if (!NearlyEqual(probs[i], it->second[i])) {
        return 1;
      }
    }
  }

  return 0;
}

