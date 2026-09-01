#include "cpu.hpp"

#include <algorithm>
#include <numeric>

namespace perfsim {

Cpu::Cpu(const CpuConfig& config, MemoryHierarchy& hierarchy)
    : hierarchy_(hierarchy), compute_cpi_(config.compute_cpi()), mshrs_(config.mshrs) {}

void Cpu::stall(uint64_t cycles, StallBucket bucket) {
  if (cycles == 0) return;
  stats_.cycles += cycles;
  switch (bucket) {
    case StallBucket::L1: stats_.stalls.l1 += cycles; break;
    case StallBucket::L2: stats_.stalls.l2 += cycles; break;
    case StallBucket::Dram: stats_.stalls.dram += cycles; break;
    case StallBucket::Bandwidth: stats_.stalls.bandwidth += cycles; break;
    case StallBucket::Mshr: stats_.stalls.mshr += cycles; break;
  }
}

void Cpu::stall_split(uint64_t cycles, StallBucket bucket, double queue_fraction) {
  if (cycles == 0) return;
  const uint64_t queued =
      static_cast<uint64_t>(static_cast<double>(cycles) * queue_fraction + 0.5);
  stall(cycles - std::min(queued, cycles), bucket);
  stall(std::min(queued, cycles), StallBucket::Bandwidth);
}

void Cpu::issue(uint64_t instructions) {
  stats_.instructions += instructions;
  compute_debt_ += static_cast<double>(instructions) * compute_cpi_;
  const uint64_t whole = static_cast<uint64_t>(compute_debt_);
  if (whole > 0) {
    compute_debt_ -= static_cast<double>(whole);
    stats_.cycles += whole;
    stats_.compute_cycles += whole;
  }
}

void Cpu::access(uint64_t address, bool is_write, bool dependent) {
  issue(1);
  ++stats_.memory_instructions;
  if (dependent) ++stats_.dependent_loads;

  const AccessOutcome outcome = hierarchy_.access(address, is_write, stats_.cycles);
  stats_.access_latency_cycles += outcome.latency;

  if (outcome.source == AccessSource::L1) {
    // An L1 hit is pipelined, so its latency only becomes visible when the very
    // next instruction needs the value.
    if (dependent) stall(outcome.latency, StallBucket::L1);
    return;
  }

  stats_.miss_latency_cycles += outcome.latency;
  const StallBucket bucket =
      outcome.source == AccessSource::L2 ? StallBucket::L2 : StallBucket::Dram;
  const double queue_fraction =
      outcome.latency == 0 ? 0.0
                           : static_cast<double>(outcome.queue_delay) /
                                 static_cast<double>(outcome.latency);

  // The miss holds an MSHR until its fill completes. Reusing the entry that
  // frees up soonest models a fully associative MSHR file.
  size_t slot = 0;
  for (size_t i = 1; i < mshrs_.size(); ++i) {
    if (mshrs_[i].free_at < mshrs_[slot].free_at) slot = i;
  }
  if (mshrs_[slot].free_at > stats_.cycles) {
    stall_split(mshrs_[slot].free_at - stats_.cycles, StallBucket::Mshr,
                mshrs_[slot].queue_fraction);
  }
  mshrs_[slot].free_at = stats_.cycles + outcome.latency;
  mshrs_[slot].bucket = bucket;
  mshrs_[slot].queue_fraction = queue_fraction;

  if (dependent) stall_split(outcome.latency, bucket, queue_fraction);
}

void Cpu::execute(const TraceRecord& record) {
  switch (record.type) {
    case RecordType::Compute:
      issue(record.value);
      break;
    case RecordType::Read:
      access(record.value, /*is_write=*/false, /*dependent=*/false);
      break;
    case RecordType::Write:
      access(record.value, /*is_write=*/true, /*dependent=*/false);
      break;
    case RecordType::DependentRead:
      access(record.value, /*is_write=*/false, /*dependent=*/true);
      break;
  }
}

void Cpu::drain() {
  // Walk the outstanding fills in completion order so the exposed tail latency
  // is charged to whichever level supplied each line.
  std::vector<size_t> order(mshrs_.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(order.begin(), order.end(),
            [this](size_t a, size_t b) { return mshrs_[a].free_at < mshrs_[b].free_at; });
  for (size_t slot : order) {
    if (mshrs_[slot].free_at > stats_.cycles) {
      stall_split(mshrs_[slot].free_at - stats_.cycles, mshrs_[slot].bucket,
                  mshrs_[slot].queue_fraction);
    }
    mshrs_[slot].free_at = 0;
  }

  // A trace shorter than the issue width would otherwise finish in zero cycles.
  if (compute_debt_ > 0.0) {
    compute_debt_ = 0.0;
    ++stats_.cycles;
    ++stats_.compute_cycles;
  }
}

}  // namespace perfsim
