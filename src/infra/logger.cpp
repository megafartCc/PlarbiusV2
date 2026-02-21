#include "plarbius/infra/logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <ostream>

namespace plarbius::infra {

namespace {

const char* LevelTag(LogLevel level) {
  switch (level) {
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarn:
      return "WARN";
    case LogLevel::kError:
      return "ERROR";
  }
  return "UNKNOWN";
}

}  // namespace

Logger::Logger(std::ostream& out) : out_(&out) {}

void Logger::Info(std::string_view message) const {
  Write(LogLevel::kInfo, message);
}

void Logger::Warn(std::string_view message) const {
  Write(LogLevel::kWarn, message);
}

void Logger::Error(std::string_view message) const {
  Write(LogLevel::kError, message);
}

void Logger::Write(LogLevel level, std::string_view message) const {
  if (out_ == nullptr) {
    return;
  }

  const auto now = std::chrono::system_clock::now();
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif

  (*out_) << '[' << std::put_time(&tm, "%H:%M:%S") << "] [" << LevelTag(level) << "] "
          << message << '\n';
}

}  // namespace plarbius::infra

