#include "workloads.hpp"

namespace perfsim::workloads {

// sum += array[random_index()].
//
// The loads miss constantly but their addresses do not depend on any loaded
// value, so the machine is free to overlap them. Comparing this against the
// pointer chase is what separates a bandwidth/MLP limit from a latency limit.
//
// Per iteration: the xorshift generator (three shifts and three xors), the index
// masking, the accumulate and the loop overhead.
constexpr uint64_t kComputePerElement = 9;

void emit_random_access(TraceWriter& out, const Params& params) {
  uint64_t state = params.seed | 1;
  const uint64_t total = params.n * params.iterations;
  for (uint64_t i = 0; i < total; ++i) {
    const uint64_t index = next_random(state) % params.n;
    out.read(kArrayBase + index * kElementSize);
    out.compute(kComputePerElement);
  }
}

uint64_t native_random_access(const Params& params) {
  std::vector<uint64_t> array(params.n);
  for (uint64_t i = 0; i < params.n; ++i) array[i] = i;

  uint64_t state = params.seed | 1;
  uint64_t sum = 0;
  const uint64_t total = params.n * params.iterations;
  for (uint64_t i = 0; i < total; ++i) {
    const uint64_t index = next_random(state) % params.n;
    sum += array[index];
  }
  return sum;
}

}  // namespace perfsim::workloads
