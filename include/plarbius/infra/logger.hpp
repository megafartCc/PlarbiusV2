#pragma once

#include <iosfwd>
#include <string_view>

namespace plarbius::infra {

enum class LogLevel {
  kInfo,
  kWarn,
  kError
};

class Logger {
 public:
  explicit Logger(std::ostream& out);

  void Info(std::string_view message) const;
  void Warn(std::string_view message) const;
  void Error(std::string_view message) const;

 private:
  void Write(LogLevel level, std::string_view message) const;
  std::ostream* out_;
};

}  // namespace plarbius::infra

