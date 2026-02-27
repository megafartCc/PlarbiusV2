#include "plarbius/infra/ipc_table_server.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace plarbius::infra {

namespace {
constexpr std::size_t kHeaderSize = sizeof(IpcSharedHeader);
constexpr std::size_t kMessageSize = sizeof(IpcMessage);

std::size_t GetTotalShmSize(std::size_t capacity) {
  // Ensure we allocate enough space for the header + N messages.
  // Add a bit of padding just in case.
  return kHeaderSize + (capacity * kMessageSize) + 64;
}
}  // namespace

// -----------------------------------------------------------------------------
// IpcTableServer
// -----------------------------------------------------------------------------

IpcTableServer::IpcTableServer(const std::string& name, std::size_t capacity)
    : name_(name), capacity_(capacity) {
  if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
    throw std::invalid_argument("IpcTableServer capacity must be a power of 2.");
  }

  const std::size_t total_size = GetTotalShmSize(capacity_);

#ifdef _WIN32
  shm_handle_ = CreateFileMappingA(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
      static_cast<DWORD>(total_size >> 32),
      static_cast<DWORD>(total_size & 0xFFFFFFFF),
      name_.c_str());
  if (!shm_handle_) {
    throw std::runtime_error("Failed to CreateFileMappingA for IPC server.");
  }

  shm_addr_ = MapViewOfFile(shm_handle_, FILE_MAP_ALL_ACCESS, 0, 0, total_size);
  if (!shm_addr_) {
    CloseHandle(shm_handle_);
    shm_handle_ = nullptr;
    throw std::runtime_error("Failed to MapViewOfFile for IPC server.");
  }
#else
  shm_unlink(name_.c_str()); // Ensure we start fresh
  shm_fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
  if (shm_fd_ < 0) {
    throw std::runtime_error("Failed to shm_open for IPC server.");
  }
  if (ftruncate(shm_fd_, static_cast<off_t>(total_size)) != 0) {
    close(shm_fd_);
    shm_unlink(name_.c_str());
    throw std::runtime_error("Failed to ftruncate SHM for IPC server.");
  }

  shm_addr_ = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
  if (shm_addr_ == MAP_FAILED) {
    close(shm_fd_);
    shm_unlink(name_.c_str());
    throw std::runtime_error("Failed to mmap SHM for IPC server.");
  }
#endif

  // Initialize header
  header_ = static_cast<IpcSharedHeader*>(shm_addr_);
  header_->head.store(0, std::memory_order_relaxed);
  header_->tail.store(0, std::memory_order_relaxed);
  header_->capacity = capacity_;

  // Messages start after the header
  char* base = static_cast<char*>(shm_addr_);
  messages_ = reinterpret_cast<IpcMessage*>(base + kHeaderSize);

  // Initialize all message states to empty
  for (std::size_t i = 0; i < capacity_; ++i) {
    messages_[i].state.store(static_cast<std::uint32_t>(IpcMessageType::kEmpty), std::memory_order_relaxed);
  }
}

IpcTableServer::~IpcTableServer() {
#ifdef _WIN32
  if (shm_addr_) {
    UnmapViewOfFile(shm_addr_);
  }
  if (shm_handle_) {
    CloseHandle(shm_handle_);
  }
#else
  if (shm_addr_ && shm_addr_ != MAP_FAILED) {
    munmap(shm_addr_, GetTotalShmSize(capacity_));
  }
  if (shm_fd_ >= 0) {
    close(shm_fd_);
    shm_unlink(name_.c_str());
  }
#endif
}

std::size_t IpcTableServer::ProcessPending(plarbius::cfr::InfosetTable& table) {
  const std::size_t mask = capacity_ - 1;
  std::size_t count = 0;
  while (true) {
    const std::uint64_t current_tail = local_tail_;
    IpcMessage& msg = messages_[current_tail & mask];
    
    // Check if the current slot has a ready message
    const auto state = static_cast<IpcMessageType>(msg.state.load(std::memory_order_acquire));
    if (state == IpcMessageType::kEmpty) {
      break; // No more pending messages
    }

    std::string key(msg.key);
    auto& node = table.GetOrCreate(key, msg.hash, msg.action_count);

    if (state == IpcMessageType::kRegretUpdate) {
      for (std::size_t i = 0; i < msg.action_count; ++i) {
        node.regret_sum[i] += msg.values[i];
      }
    } else if (state == IpcMessageType::kStrategyUpdate) {
      for (std::size_t i = 0; i < msg.action_count; ++i) {
        node.strategy_sum[i] += msg.values[i];
      }
      node.visit_count.fetch_add(1, std::memory_order_relaxed);
    }

    // Mark slot as empty for producers to reuse
    msg.state.store(static_cast<std::uint32_t>(IpcMessageType::kEmpty), std::memory_order_release);
    local_tail_ = current_tail + 1;
    ++count;
  }
  // Optional: Update the published tail so producers know how much space is free.
  // We can just loosely update it to reduce contention.
  header_->tail.store(local_tail_, std::memory_order_release);
  return count;
}

// -----------------------------------------------------------------------------
// IpcTableClient
// -----------------------------------------------------------------------------

IpcTableClient::IpcTableClient(const std::string& name) : name_(name) {
#ifdef _WIN32
  shm_handle_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name_.c_str());
  if (!shm_handle_) {
    throw std::runtime_error("Failed to OpenFileMappingA for IPC client: " + name_);
  }

  // Map a small portion first just to read the header and know the capacity
  void* temp_addr = MapViewOfFile(shm_handle_, FILE_MAP_ALL_ACCESS, 0, 0, kHeaderSize);
  if (!temp_addr) {
    CloseHandle(shm_handle_);
    throw std::runtime_error("Failed to MapViewOfFile temp for IPC client.");
  }
  capacity_ = static_cast<IpcSharedHeader*>(temp_addr)->capacity;
  UnmapViewOfFile(temp_addr);

  const std::size_t total_size = GetTotalShmSize(capacity_);
  shm_addr_ = MapViewOfFile(shm_handle_, FILE_MAP_ALL_ACCESS, 0, 0, total_size);
  if (!shm_addr_) {
    CloseHandle(shm_handle_);
    throw std::runtime_error("Failed to MapViewOfFile full for IPC client.");
  }
#else
  shm_fd_ = shm_open(name_.c_str(), O_RDWR, 0666);
  if (shm_fd_ < 0) {
    throw std::runtime_error("Failed to shm_open for IPC client: " + name_);
  }
  
  // Read capacity from header
  IpcSharedHeader temp_header;
  if (pread(shm_fd_, &temp_header, sizeof(temp_header), 0) != sizeof(temp_header)) {
    close(shm_fd_);
    throw std::runtime_error("Failed to read header capacity for IPC client.");
  }
  capacity_ = temp_header.capacity;

  const std::size_t total_size = GetTotalShmSize(capacity_);
  shm_addr_ = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
  if (shm_addr_ == MAP_FAILED) {
    close(shm_fd_);
    throw std::runtime_error("Failed to mmap SHM for IPC client.");
  }
#endif

  header_ = static_cast<IpcSharedHeader*>(shm_addr_);
  char* base = static_cast<char*>(shm_addr_);
  messages_ = reinterpret_cast<IpcMessage*>(base + kHeaderSize);
}

IpcTableClient::~IpcTableClient() {
#ifdef _WIN32
  if (shm_addr_) {
    UnmapViewOfFile(shm_addr_);
  }
  if (shm_handle_) {
    CloseHandle(shm_handle_);
  }
#else
  if (shm_addr_ && shm_addr_ != MAP_FAILED) {
    munmap(shm_addr_, GetTotalShmSize(capacity_));
  }
  if (shm_fd_ >= 0) {
    close(shm_fd_);
  }
#endif
}

void IpcTableClient::PushMessage(std::uint32_t type, const std::string& key, std::uint64_t hash, std::size_t action_count, const double* values) {
  const std::size_t mask = capacity_ - 1;
  
  std::uint64_t head;
  while (true) {
    head = header_->head.load(std::memory_order_relaxed);
    const std::uint64_t tail = header_->tail.load(std::memory_order_acquire);
    
    // Check if the queue is full
    if (head - tail >= capacity_) {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
      continue;
    }

    // Try to claim this slot
    if (header_->head.compare_exchange_weak(head, head + 1, std::memory_order_acquire)) {
      break;
    }
  }

  // We successfully claimed `head`. Now fill the message in the slot.
  IpcMessage& msg = messages_[head & mask];
  
  // As an extra sanity check, wait until the message actually becomes empty.
  // This handles the case where the server hasn't updated the state yet physically.
  while (msg.state.load(std::memory_order_acquire) != static_cast<std::uint32_t>(IpcMessageType::kEmpty)) {
    std::this_thread::sleep_for(std::chrono::microseconds(5));
  }

  const std::size_t copy_len = std::min(key.size(), sizeof(msg.key) - 1);
  std::memcpy(msg.key, key.data(), copy_len);
  msg.key[copy_len] = '\0';
  msg.hash = hash;
  
  const std::size_t copy_actions = std::min(action_count, static_cast<std::size_t>(16));
  msg.action_count = copy_actions;
  if (copy_actions > 0 && values) {
    std::memcpy(msg.values, values, copy_actions * sizeof(double));
  }

  // Publish message to the server
  msg.state.store(type, std::memory_order_release);
}

void IpcTableClient::SendRegretUpdate(const std::string& key, std::uint64_t hash, std::size_t action_count, const double* regrets) {
  PushMessage(static_cast<std::uint32_t>(IpcMessageType::kRegretUpdate), key, hash, action_count, regrets);
}

void IpcTableClient::SendStrategyUpdate(const std::string& key, std::uint64_t hash, std::size_t action_count, const double* strategies) {
  PushMessage(static_cast<std::uint32_t>(IpcMessageType::kStrategyUpdate), key, hash, action_count, strategies);
}

}  // namespace plarbius::infra
