#include "workloads.hpp"

#include <algorithm>

namespace perfsim::workloads {

// node = next[node], following a single random cycle through the array.
//
// Every load's address is the previous load's result, so the machine cannot
// start the next miss until the current one has returned. Miss rates match the
// random-access workload but performance does not, which is the whole point.
//
// Per hop: the loop counter increment and the compare-and-branch.
constexpr uint64_t kComputePerHop = 2;

std::vector<uint64_t> build_chase_permutation(uint64_t n, uint64_t seed) {
  std::vector<uint64_t> next(n);
  for (uint64_t i = 0; i < n; ++i) next[i] = i;

  // Sattolo's algorithm produces a permutation that is a single cycle covering
  // every element, so the chase can never fall into a short loop.
  uint64_t state = seed | 1;
  for (uint64_t i = n - 1; i > 0; --i) {
    const uint64_t j = next_random(state) % i;
    std::swap(next[i], next[j]);
  }
  return next;
}

void emit_pointer_chase(TraceWriter& out, const Params& params) {
  const std::vector<uint64_t> next = build_chase_permutation(params.n, params.seed);
  uint64_t node = 0;
  const uint64_t hops = params.n * params.iterations;
  for (uint64_t i = 0; i < hops; ++i) {
    out.dependent_read(kArrayBase + node * kElementSize);
    out.compute(kComputePerHop);
    node = next[node];
  }
}

uint64_t native_pointer_chase(const Params& params) {
  const std::vector<uint64_t> next = build_chase_permutation(params.n, params.seed);
  uint64_t node = 0;
  uint64_t visited = 0;
  const uint64_t hops = params.n * params.iterations;
  for (uint64_t i = 0; i < hops; ++i) {
    node = next[node];
    visited += node;
  }
  return visited;
}

}  // namespace perfsim::workloads
