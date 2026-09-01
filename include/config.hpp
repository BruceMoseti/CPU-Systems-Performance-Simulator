// Architecture description: everything the simulated machine is made of.
#pragma once

#include <cstdint>
#include <string>

#include "json.hpp"

namespace perfsim {

struct CacheConfig {
  std::string name;
  bool enabled = true;
  uint32_t size_kb = 32;
  uint32_t line_size = 64;
  uint32_t associativity = 8;
  uint32_t latency_cycles = 4;

  uint64_t size_bytes() const { return uint64_t(size_kb) * 1024; }
  uint64_t line_count() const { return size_bytes() / line_size; }
  uint64_t set_count() const { return line_count() / associativity; }
};

struct MemoryConfig {
  uint32_t latency_cycles = 180;
  double bandwidth_gbps = 50.0;  // GB/s, used to derive per-cycle bus occupancy
};

struct CpuConfig {
  double frequency_ghz = 3.5;
  uint32_t issue_width = 4;

  // Non-memory CPI floor. The simulator models memory stalls explicitly but not
  // ALU dependence chains or branch mispredictions; base_cpi is the knob for
  // folding those in when calibrating against real hardware. 0 means "ideal
  // front end", in which case the issue width alone limits compute throughput.
  double base_cpi = 0.0;

  // Miss Status Holding Registers: how many cache misses may be outstanding at
  // once. This is the memory-level parallelism limit and is what separates a
  // latency-bound workload from a throughput-bound one.
  uint32_t mshrs = 16;

  double compute_cpi() const {
    const double issue_limit = 1.0 / static_cast<double>(issue_width);
    return base_cpi > issue_limit ? base_cpi : issue_limit;
  }
};

struct Config {
  CpuConfig cpu;
  CacheConfig l1;
  CacheConfig l2;
  MemoryConfig memory;

  Config();

  // Rejects unknown keys so that a mistyped sweep parameter fails loudly
  // instead of silently producing results for the wrong machine.
  static Config from_json(const Json& root);
  static Config from_file(const std::string& path);

  Json to_json() const;
};

}  // namespace perfsim
