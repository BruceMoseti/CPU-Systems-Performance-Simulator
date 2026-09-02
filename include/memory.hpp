// Main memory model and the L1 -> L2 -> DRAM hierarchy that sits under the CPU.
#pragma once

#include <cstdint>
#include <optional>

#include "cache.hpp"
#include "config.hpp"

namespace perfsim {

struct MemoryStats {
  uint64_t reads = 0;
  uint64_t writes = 0;
  uint64_t bytes_read = 0;
  uint64_t bytes_written = 0;
  uint64_t queue_delay_cycles = 0;
};

struct MemoryResult {
  uint64_t latency = 0;      // includes any queueing delay
  uint64_t queue_delay = 0;  // portion of latency caused by bus contention
};

// Fixed-latency DRAM with a bandwidth limit.
//
// The model is deliberately address-independent: there are no banks, rows or
// refresh cycles. Bandwidth is enforced by tracking when the bus next becomes
// free, so a workload that requests more bytes per cycle than the bus can carry
// sees its effective latency grow. That is enough to make bandwidth appear as a
// distinct bottleneck without simulating DRAM timing protocols.
class Memory {
 public:
  Memory(const MemoryConfig& config, uint32_t transfer_bytes, double frequency_ghz);

  MemoryResult read(uint64_t request_cycle);
  // Writebacks consume bandwidth but are never on the critical path, so they
  // return no latency.
  void write(uint64_t request_cycle);

  const MemoryStats& stats() const { return stats_; }
  double bytes_per_cycle() const { return bytes_per_cycle_; }
  uint32_t transfer_bytes() const { return transfer_bytes_; }

 private:
  uint64_t occupy_bus(uint64_t request_cycle);

  MemoryConfig config_;
  uint32_t transfer_bytes_;
  double bytes_per_cycle_;
  double transfer_cycles_;
  double bus_free_cycle_ = 0.0;
  MemoryStats stats_;
};

enum class AccessSource : uint8_t { L1, L2, Memory };

struct AccessOutcome {
  AccessSource source = AccessSource::L1;
  uint64_t latency = 0;
  uint64_t queue_delay = 0;
};

// Cache levels are looked up in series, so a hit in L2 costs the L1 latency
// plus the L2 latency. That matches how most designs behave and keeps the
// latency arithmetic easy to reason about.
class MemoryHierarchy {
 public:
  explicit MemoryHierarchy(const Config& config);

  AccessOutcome access(uint64_t address, bool is_write, uint64_t cycle);

  const Cache& l1() const { return l1_; }
  const Cache* l2() const { return l2_.has_value() ? &*l2_ : nullptr; }
  const Memory& memory() const { return memory_; }

 private:
  void writeback_from_l1(uint64_t address, uint64_t cycle);

  Cache l1_;
  std::optional<Cache> l2_;
  Memory memory_;
  uint32_t l1_latency_;
  uint32_t l2_latency_;
};

}  // namespace perfsim
