// Synthetic workloads with deliberately different locality characteristics.
//
// Every workload exists in two forms built from the same parameters:
//
//   emit()   writes the memory/instruction trace the simulator consumes
//   native() runs the real kernel so the model can be compared against hardware
//
// Keeping both in one file per workload is what stops the trace and the real
// kernel from drifting apart.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "trace.hpp"

namespace perfsim::workloads {

struct Params {
  uint64_t n = 1 << 20;      // elements, or matrix dimension for matrix kernels
  uint64_t block = 0;        // matrix blocking factor; 0 or >= n means unblocked
  uint64_t stride = 4096;    // byte spacing between elements for the strided kernel
  uint64_t iterations = 1;   // repeat the kernel to reach a steady state
  uint64_t seed = 12345;
};

struct Workload {
  const char* name;
  const char* description;
  void (*emit)(TraceWriter& out, const Params& params);
  // Returns a checksum so that the optimiser cannot delete the kernel.
  uint64_t (*native)(const Params& params);
};

const std::vector<Workload>& registry();
const Workload* find(const std::string& name);

// Element size shared by every workload, so trace addresses and the native
// kernels agree on stride.
constexpr uint64_t kElementSize = 8;

// Base addresses are page aligned and far apart so that arrays do not alias by
// accident in a way the real kernels would not.
constexpr uint64_t kArrayBase = 0x1000000;

inline uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// Shared generator so that native and trace modes visit identical addresses.
inline uint64_t next_random(uint64_t& state) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

void emit_sequential(TraceWriter& out, const Params& params);
uint64_t native_sequential(const Params& params);
void emit_random_access(TraceWriter& out, const Params& params);
uint64_t native_random_access(const Params& params);
void emit_pointer_chase(TraceWriter& out, const Params& params);
uint64_t native_pointer_chase(const Params& params);
void emit_strided(TraceWriter& out, const Params& params);
uint64_t native_strided(const Params& params);
void emit_matrix(TraceWriter& out, const Params& params);
uint64_t native_matrix(const Params& params);

// Builds the single-cycle permutation used by the pointer chase, identically in
// both modes.
std::vector<uint64_t> build_chase_permutation(uint64_t n, uint64_t seed);

}  // namespace perfsim::workloads
