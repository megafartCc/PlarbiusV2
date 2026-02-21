#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "plarbius/cfr/infoset_checkpoint_io.hpp"

namespace {

bool NearlyEqual(double a, double b, double epsilon = 1e-12) {
  return std::fabs(a - b) <= epsilon;
}

}  // namespace

int main() {
  plarbius::cfr::InfosetTable original;
  original.Put("0|J|_", plarbius::cfr::InfosetNode{
                            std::vector<double>{1.5, 0.0},
                            std::vector<double>{10.0, 30.0},
                        });
  original.Put("1|Q|b", plarbius::cfr::InfosetNode{
                            std::vector<double>{0.0, 2.0},
                            std::vector<double>{7.0, 13.0},
                        });

  const std::filesystem::path checkpoint =
      std::filesystem::temp_directory_path() / "plarbius_checkpoint_io_test.tsv";
  plarbius::cfr::InfosetCheckpointIo::Save(original, checkpoint.string());

  plarbius::cfr::InfosetTable loaded;
  plarbius::cfr::InfosetCheckpointIo::Load(loaded, checkpoint.string());

  std::filesystem::remove(checkpoint);

  if (loaded.Size() != original.Size()) {
    return 1;
  }

  for (const auto& [key, source_node] : original.Nodes()) {
    const auto it = loaded.Nodes().find(key);
    if (it == loaded.Nodes().end()) {
      return 1;
    }
    if (it->second.regret_sum.size() != source_node.regret_sum.size() ||
        it->second.strategy_sum.size() != source_node.strategy_sum.size()) {
      return 1;
    }
    for (std::size_t i = 0; i < source_node.regret_sum.size(); ++i) {
      if (!NearlyEqual(it->second.regret_sum[i], source_node.regret_sum[i])) {
        return 1;
      }
      if (!NearlyEqual(it->second.strategy_sum[i], source_node.strategy_sum[i])) {
        return 1;
      }
    }
  }

  return 0;
}
