#pragma once

#include <cstdint>
#include <string>

namespace plarbius::games::hunl {

struct HunlBucketConfig {
  std::uint32_t preflop_private_buckets = 24;
  std::uint32_t flop_private_buckets = 48;
  std::uint32_t turn_private_buckets = 40;
  std::uint32_t river_private_buckets = 32;
  std::uint32_t flop_public_buckets = 16;
  std::uint32_t turn_public_buckets = 12;
  std::uint32_t river_public_buckets = 10;
  std::uint32_t chance_outcomes = 128;
};

HunlBucketConfig DefaultHunlBucketConfig();
HunlBucketConfig LoadHunlBucketConfig(const std::string& path);
void SaveHunlBucketConfig(const HunlBucketConfig& config, const std::string& path);

}  // namespace plarbius::games::hunl
