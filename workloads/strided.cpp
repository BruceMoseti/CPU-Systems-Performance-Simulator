#include "workloads.hpp"

namespace perfsim::workloads {

// Random loads into an array of n structures that are `stride` bytes apart,
// touching one field of each.
//
// Only one cache line per structure is ever read, so the touched data is far
// smaller than any cache here: capacity is never the constraint. But when the
// stride is a multiple of the set-index period, every structure maps to the same
// set, and then associativity alone decides whether an access hits. That is what
// a conflict miss is, and it is why padding a stride is standard advice.
//
// Per access: the random index, scaling it by the stride, the accumulate and the
// loop overhead.
constexpr uint64_t kComputePerAccess = 6;

void emit_strided(TraceWriter& out, const Params& params) {
  uint64_t state = params.seed | 1;
  const uint64_t total = params.n * params.iterations;
  for (uint64_t i = 0; i < total; ++i) {
    const uint64_t index = next_random(state) % params.n;
    out.read(kArrayBase + index * params.stride);
    out.compute(kComputePerAccess);
  }
}

NativeResult native_strided(const Params& params) {
  const uint64_t step = params.stride / kElementSize;
  std::vector<uint64_t> array(params.n * step);
  for (uint64_t i = 0; i < array.size(); ++i) array[i] = i;

  return time_kernel([&] {
    uint64_t state = params.seed | 1;
    uint64_t sum = 0;
    const uint64_t total = params.n * params.iterations;
    for (uint64_t i = 0; i < total; ++i) {
      const uint64_t index = next_random(state) % params.n;
      sum += array[index * step];
    }
    return sum;
  });
}

}  // namespace perfsim::workloads
