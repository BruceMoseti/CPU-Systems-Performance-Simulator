#include "memory.hpp"

#include <algorithm>
#include <cmath>

namespace perfsim {

Memory::Memory(const MemoryConfig& config, uint32_t transfer_bytes, double frequency_ghz)
    : config_(config),
      transfer_bytes_(transfer_bytes),
      // GB/s divided by GHz gives bytes per cycle.
      bytes_per_cycle_(config.bandwidth_gbps / frequency_ghz),
      transfer_cycles_(static_cast<double>(transfer_bytes) /
                       (config.bandwidth_gbps / frequency_ghz)) {}

uint64_t Memory::occupy_bus(uint64_t request_cycle) {
  const double arrival = static_cast<double>(request_cycle);
  const double start = std::max(bus_free_cycle_, arrival);
  bus_free_cycle_ = start + transfer_cycles_;
  const double delay = start - arrival;
  const uint64_t rounded = static_cast<uint64_t>(std::llround(std::max(0.0, delay)));
  stats_.queue_delay_cycles += rounded;
  return rounded;
}

MemoryResult Memory::read(uint64_t request_cycle) {
  ++stats_.reads;
  stats_.bytes_read += transfer_bytes_;
  MemoryResult result;
  result.queue_delay = occupy_bus(request_cycle);
  result.latency = config_.latency_cycles + result.queue_delay;
  return result;
}

void Memory::write(uint64_t request_cycle) {
  ++stats_.writes;
  stats_.bytes_written += transfer_bytes_;
  occupy_bus(request_cycle);
}

MemoryHierarchy::MemoryHierarchy(const Config& config)
    : l1_(config.l1),
      memory_(config.memory,
              config.l2.enabled ? config.l2.line_size : config.l1.line_size,
              config.cpu.frequency_ghz),
      l1_latency_(config.l1.latency_cycles),
      l2_latency_(config.l2.enabled ? config.l2.latency_cycles : 0) {
  if (config.l2.enabled) l2_.emplace(config.l2);
}

void MemoryHierarchy::writeback_from_l1(uint64_t address, uint64_t cycle) {
  if (!l2_.has_value()) {
    memory_.write(cycle);
    return;
  }
  const CacheResult result = l2_->install_writeback(address);
  if (result.dirty_eviction) memory_.write(cycle);
}

AccessOutcome MemoryHierarchy::access(uint64_t address, bool is_write, uint64_t cycle) {
  AccessOutcome outcome;
  const CacheResult l1_result = l1_.access(address, is_write);
  outcome.latency = l1_latency_;
  if (l1_result.hit) {
    outcome.source = AccessSource::L1;
    return outcome;
  }

  if (l2_.has_value()) {
    // A fill request into L2 is a read regardless of whether the CPU was
    // storing: the line is fetched first, then modified in L1.
    const CacheResult l2_result = l2_->access(address, /*is_write=*/false);
    outcome.latency += l2_latency_;
    if (l2_result.hit) {
      outcome.source = AccessSource::L2;
    } else {
      const MemoryResult dram = memory_.read(cycle + outcome.latency);
      outcome.latency += dram.latency;
      outcome.queue_delay = dram.queue_delay;
      outcome.source = AccessSource::Memory;
      if (l2_result.dirty_eviction) memory_.write(cycle);
    }
  } else {
    const MemoryResult dram = memory_.read(cycle + outcome.latency);
    outcome.latency += dram.latency;
    outcome.queue_delay = dram.queue_delay;
    outcome.source = AccessSource::Memory;
  }

  if (l1_result.dirty_eviction) writeback_from_l1(l1_result.evicted_address, cycle);
  return outcome;
}

}  // namespace perfsim
