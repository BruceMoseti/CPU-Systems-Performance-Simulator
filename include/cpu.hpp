// CPU timing model.
//
// The model is not cycle-accurate and does not simulate a pipeline. It tracks a
// single cycle counter and charges it for two things:
//
//   1. Issue bandwidth. Every instruction costs compute_cpi cycles, so a 4-wide
//      machine retires four instructions per cycle when nothing is stalling.
//
//   2. Exposed memory latency. Misses are non-blocking: an independent miss is
//      overlapped with later work and costs nothing directly. It does occupy an
//      MSHR until its fill completes, so once all MSHRs are busy the CPU stalls
//      and memory-level parallelism becomes the limit. A dependent load (record
//      type 'L') cannot be overlapped and exposes its entire latency.
//
// That is what makes a pointer chase and a random walk behave differently even
// when their miss rates are identical.
#pragma once

#include <cstdint>
#include <vector>

#include "config.hpp"
#include "memory.hpp"
#include "trace.hpp"

namespace perfsim {

// Every stalled cycle lands in exactly one bucket, so the buckets sum to the
// total stall count. Cycles are attributed to the level that supplied the data,
// which means "dram" reads as "cycles lost to accesses that went to DRAM".
enum class StallBucket : uint8_t { L1, L2, Dram, Bandwidth, Mshr };

struct StallCycles {
  uint64_t l1 = 0;
  uint64_t l2 = 0;
  uint64_t dram = 0;
  uint64_t bandwidth = 0;  // queueing for the DRAM bus rather than raw latency
  uint64_t mshr = 0;       // all MSHRs busy: no more misses could be started

  uint64_t total() const { return l1 + l2 + dram + bandwidth + mshr; }
};

struct CpuStats {
  uint64_t instructions = 0;
  uint64_t memory_instructions = 0;
  uint64_t dependent_loads = 0;
  uint64_t cycles = 0;
  uint64_t compute_cycles = 0;
  StallCycles stalls;
  uint64_t access_latency_cycles = 0;  // summed over every memory instruction
  uint64_t miss_latency_cycles = 0;    // summed over accesses that missed in L1
};

class Cpu {
 public:
  Cpu(const CpuConfig& config, MemoryHierarchy& hierarchy);

  void execute(const TraceRecord& record);

  // Lets outstanding misses complete and flushes partial issue cycles. Must be
  // called once the trace is exhausted, before reading the statistics.
  void drain();

  const CpuStats& stats() const { return stats_; }

 private:
  struct Mshr {
    uint64_t free_at = 0;
    StallBucket bucket = StallBucket::Dram;
    // Share of this fill's latency that was spent waiting for the DRAM bus.
    // Cycles lost behind this entry are split accordingly, so a bandwidth
    // problem is reported as one instead of hiding inside the MSHR bucket.
    double queue_fraction = 0.0;
  };

  // Fractional cycles are carried in fixed point, in units of 2^-32 of a cycle.
  // Integer arithmetic keeps the hottest function in the simulator free of a
  // double-to-integer conversion, which is undefined rather than merely
  // inaccurate once the value exceeds the destination range. It is also exact
  // for every power-of-two issue width, which is the common case.
  static constexpr int kCpiFractionBits = 32;

  void issue(uint64_t instructions);
  void access(uint64_t address, bool is_write, bool dependent);
  void stall(uint64_t cycles, StallBucket bucket);
  void stall_split(uint64_t cycles, StallBucket bucket, double queue_fraction);
  void sift_down(size_t root);

  MemoryHierarchy& hierarchy_;
  uint64_t compute_cpi_scaled_;
  // Cycle debt carried between instructions so that, for example, four
  // instructions on a 4-wide machine cost exactly one cycle.
  uint64_t compute_debt_ = 0;
  // A binary min-heap on free_at, so the entry that frees up soonest is always
  // at index 0. Every miss needs that entry, and a linear scan over it was the
  // single hottest loop in the simulator. MSHRs are interchangeable, so which
  // slot a miss lands in has no effect on the results.
  std::vector<Mshr> mshrs_;
  CpuStats stats_;
};

}  // namespace perfsim
