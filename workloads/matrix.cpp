#include "workloads.hpp"

#include <algorithm>

namespace perfsim::workloads {
namespace {

// The multiply, the accumulate and the inner loop's compare-and-branch.
constexpr uint64_t kComputePerMultiply = 3;

struct Layout {
  uint64_t a;
  uint64_t b;
  uint64_t c;
};

Layout layout_for(uint64_t n) {
  const uint64_t bytes = n * n * kElementSize;
  Layout layout;
  layout.a = kArrayBase;
  layout.b = align_up(layout.a + bytes, 4096);
  layout.c = align_up(layout.b + bytes, 4096);
  return layout;
}

// 0 or a value at least n means "no blocking", which yields the textbook
// i/j/k loop nest. One implementation covers both variants, so naive and blocked
// runs perform exactly the same arithmetic and only differ in access order.
uint64_t effective_block(const Params& params) {
  if (params.block == 0 || params.block >= params.n) return params.n;
  return params.block;
}

}  // namespace

void emit_matrix(TraceWriter& out, const Params& params) {
  const uint64_t n = params.n;
  const uint64_t block = effective_block(params);
  const Layout layout = layout_for(n);

  for (uint64_t iteration = 0; iteration < params.iterations; ++iteration) {
    for (uint64_t ii = 0; ii < n; ii += block) {
      for (uint64_t jj = 0; jj < n; jj += block) {
        for (uint64_t kk = 0; kk < n; kk += block) {
          const uint64_t i_end = std::min(ii + block, n);
          const uint64_t j_end = std::min(jj + block, n);
          const uint64_t k_end = std::min(kk + block, n);
          for (uint64_t i = ii; i < i_end; ++i) {
            for (uint64_t j = jj; j < j_end; ++j) {
              out.read(layout.c + (i * n + j) * kElementSize);
              for (uint64_t k = kk; k < k_end; ++k) {
                out.read(layout.a + (i * n + k) * kElementSize);
                out.read(layout.b + (k * n + j) * kElementSize);
                out.compute(kComputePerMultiply);
              }
              out.write(layout.c + (i * n + j) * kElementSize);
            }
          }
        }
      }
    }
  }
}

NativeResult native_matrix(const Params& params) {
  const uint64_t n = params.n;
  const uint64_t block = effective_block(params);

  // Integer elements keep the result bit-identical between the naive and
  // blocked variants, which makes a mismatched checksum a real bug signal.
  std::vector<uint64_t> a(n * n);
  std::vector<uint64_t> b(n * n);
  std::vector<uint64_t> c(n * n, 0);
  uint64_t state = params.seed | 1;
  for (uint64_t i = 0; i < n * n; ++i) {
    a[i] = next_random(state) & 0xFFFF;
    b[i] = next_random(state) & 0xFFFF;
  }

  NativeResult result = time_kernel([&] {
    for (uint64_t iteration = 0; iteration < params.iterations; ++iteration) {
      for (uint64_t ii = 0; ii < n; ii += block) {
        for (uint64_t jj = 0; jj < n; jj += block) {
          for (uint64_t kk = 0; kk < n; kk += block) {
            const uint64_t i_end = std::min(ii + block, n);
            const uint64_t j_end = std::min(jj + block, n);
            const uint64_t k_end = std::min(kk + block, n);
            for (uint64_t i = ii; i < i_end; ++i) {
              for (uint64_t j = jj; j < j_end; ++j) {
                uint64_t sum = c[i * n + j];
                for (uint64_t k = kk; k < k_end; ++k) {
                  sum += a[i * n + k] * b[k * n + j];
                }
                c[i * n + j] = sum;
              }
            }
          }
        }
      }
    }
    return uint64_t{0};
  });

  for (uint64_t i = 0; i < n * n; ++i) result.checksum += c[i];
  return result;
}

}  // namespace perfsim::workloads
