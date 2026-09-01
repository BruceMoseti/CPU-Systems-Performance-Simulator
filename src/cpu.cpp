#include "cpu.hpp"

#include <algorithm>

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
  const uint64_t queued = std::min(
      cycles, static_cast<uint64_t>(static_cast<double>(cycles) * queue_fraction + 0.5));
  stall(cycles - queued, bucket);
  stall(queued, StallBucket::Bandwidth);
}

void Cpu::sift_down(size_t root) {
  const size_t count = mshrs_.size();
  while (true) {
    size_t smallest = root;
    const size_t left = 2 * root + 1;
    const size_t right = left + 1;
    if (left < count && mshrs_[left].free_at < mshrs_[smallest].free_at) smallest = left;
    if (right < count && mshrs_[right].free_at < mshrs_[smallest].free_at) smallest = right;
    if (smallest == root) return;
    std::swap(mshrs_[root], mshrs_[smallest]);
    root = smallest;
  }
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
  const uint64_t earliest_free = mshrs_[0].free_at;
  if (earliest_free > stats_.cycles) {
    stall_split(earliest_free - stats_.cycles, StallBucket::Mshr, mshrs_[0].queue_fraction);
  }
  mshrs_[0] = Mshr{stats_.cycles + outcome.latency, bucket, queue_fraction};
  sift_down(0);

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
  // is charged to whichever level supplied each line. Sorting breaks the heap
  // ordering, which is fine because every entry is cleared straight afterwards.
  std::sort(mshrs_.begin(), mshrs_.end(),
            [](const Mshr& a, const Mshr& b) { return a.free_at < b.free_at; });
  for (Mshr& mshr : mshrs_) {
    if (mshr.free_at > stats_.cycles) {
      stall_split(mshr.free_at - stats_.cycles, mshr.bucket, mshr.queue_fraction);
    }
    mshr.free_at = 0;
  }

  // A trace shorter than the issue width would otherwise finish in zero cycles.
  if (compute_debt_ > 0.0) {
    compute_debt_ = 0.0;
    ++stats_.cycles;
    ++stats_.compute_cycles;
  }
}

}  // namespace perfsim
