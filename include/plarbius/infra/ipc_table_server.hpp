#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "plarbius/cfr/infoset_table.hpp"

namespace plarbius::infra {

enum class IpcMessageType : std::uint32_t {
  kEmpty = 0,
  kRegretUpdate = 1,
  kStrategyUpdate = 2
};

struct IpcMessage {
  std::atomic<std::uint32_t> state;
  char key[128];
  std::uint64_t hash;
  std::size_t action_count;
  double values[16];
};

struct IpcSharedHeader {
  std::atomic<std::uint64_t> head;
  std::atomic<std::uint64_t> tail;
  std::size_t capacity;
};

class IpcTableServer {
 public:
  IpcTableServer(const std::string& name, std::size_t capacity);
  ~IpcTableServer();

  // Non-copyable/movable
  IpcTableServer(const IpcTableServer&) = delete;
  IpcTableServer& operator=(const IpcTableServer&) = delete;

  std::size_t ProcessPending(plarbius::cfr::InfosetTable& table);

 private:
  std::string name_;
  std::size_t capacity_;
  void* shm_addr_ = nullptr;
#ifdef _WIN32
  void* shm_handle_ = nullptr;
#else
  int shm_fd_ = -1;
#endif
  IpcSharedHeader* header_ = nullptr;
  IpcMessage* messages_ = nullptr;
  std::uint64_t local_tail_ = 0;
};

class IpcTableClient {
 public:
  explicit IpcTableClient(const std::string& name);
  ~IpcTableClient();

  // Non-copyable/movable
  IpcTableClient(const IpcTableClient&) = delete;
  IpcTableClient& operator=(const IpcTableClient&) = delete;

  void SendRegretUpdate(const std::string& key, std::uint64_t hash, std::size_t action_count, const double* regrets);
  void SendStrategyUpdate(const std::string& key, std::uint64_t hash, std::size_t action_count, const double* strategies);

 private:
  void PushMessage(std::uint32_t type, const std::string& key, std::uint64_t hash, std::size_t action_count, const double* values);

  std::string name_;
  std::size_t capacity_ = 0;
  void* shm_addr_ = nullptr;
#ifdef _WIN32
  void* shm_handle_ = nullptr;
#else
  int shm_fd_ = -1;
#endif
  IpcSharedHeader* header_ = nullptr;
  IpcMessage* messages_ = nullptr;
};

}  // namespace plarbius::infra
