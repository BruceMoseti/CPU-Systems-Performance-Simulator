#include "workloads.hpp"

namespace perfsim::workloads {

// sum += array[i] for every element, in order.
//
// Each iteration is one load plus the loop's own arithmetic: the add, the index
// increment and the compare-and-branch.
constexpr uint64_t kComputePerElement = 3;

void emit_sequential(TraceWriter& out, const Params& params) {
  for (uint64_t iteration = 0; iteration < params.iterations; ++iteration) {
    for (uint64_t i = 0; i < params.n; ++i) {
      out.read(kArrayBase + i * kElementSize);
      out.compute(kComputePerElement);
    }
  }
}

uint64_t native_sequential(const Params& params) {
  std::vector<uint64_t> array(params.n);
  for (uint64_t i = 0; i < params.n; ++i) array[i] = i;

  uint64_t sum = 0;
  for (uint64_t iteration = 0; iteration < params.iterations; ++iteration) {
    for (uint64_t i = 0; i < params.n; ++i) sum += array[i];
  }
  return sum;
}

}  // namespace perfsim::workloads
