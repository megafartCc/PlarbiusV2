#pragma once

#include <string>

#include "plarbius/cfr/infoset_table.hpp"

namespace plarbius::cfr {

class InfosetCheckpointIo {
 public:
  static void Save(const InfosetTable& table, const std::string& path);
  static void Load(InfosetTable& table, const std::string& path);
};

}  // namespace plarbius::cfr

