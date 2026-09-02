// Set-associative, write-back, write-allocate cache with LRU replacement.
#pragma once

#include <cstdint>
#include <vector>

#include "config.hpp"

namespace perfsim {

struct CacheStats {
  uint64_t accesses = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t writebacks = 0;     // dirty lines sent to the next level
  uint64_t writebacks_in = 0;  // dirty lines received from the level above

  double hit_rate() const {
    return accesses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(accesses);
  }
  double miss_rate() const { return accesses == 0 ? 0.0 : 1.0 - hit_rate(); }
};

struct CacheResult {
  bool hit = false;
  bool dirty_eviction = false;
  uint64_t evicted_address = 0;
};

class Cache {
 public:
  explicit Cache(const CacheConfig& config);

  // A demand access from the level above. Counted in the hit/miss statistics.
  CacheResult access(uint64_t address, bool is_write);

  // A dirty line evicted from the level above. This is not a demand access, so
  // it is deliberately kept out of the hit/miss statistics: mixing writebacks
  // into the hit rate would make the reported rate depend on eviction timing
  // rather than on the workload's locality.
  CacheResult install_writeback(uint64_t address);

  const CacheConfig& config() const { return config_; }
  const CacheStats& stats() const { return stats_; }

 private:
  // Line state is held as parallel arrays rather than an array of structs. The
  // tag scan is the simulator's hottest loop, and this way the tags of one set
  // are contiguous: an 8-way lookup reads a single 64-byte host cache line
  // instead of striding over three.
  //
  // An empty way is marked by an all-ones tag. Because line_size is at least 4,
  // a real line address is address >> 2 or smaller and so always has its top two
  // bits clear, which makes all-ones unreachable and safe as a sentinel.
  static constexpr uint64_t kEmptyTag = ~uint64_t{0};

  uint64_t set_index(uint64_t line_address) const {
    return set_mask_ != 0 ? (line_address & set_mask_) : line_address % set_count_;
  }

  // Returns the way holding line_address within its set, or -1 when absent.
  int find_way(size_t base, uint64_t line_address) const;
  // Empty ways carry timestamp 0, below every real one, so choosing the oldest
  // timestamp naturally prefers an empty way and needs no separate check.
  int select_victim(size_t base) const;
  // Shared tail of access() and install_writeback() for the miss path.
  CacheResult replace(size_t base, uint64_t line_address, bool dirty);

  CacheConfig config_;
  uint64_t line_shift_ = 0;
  uint64_t set_count_ = 0;
  uint64_t set_mask_ = 0;  // non-zero only when set_count_ is a power of two
  uint32_t associativity_ = 1;
  uint64_t tick_ = 0;
  std::vector<uint64_t> tags_;
  std::vector<uint64_t> used_;
  std::vector<uint8_t> dirty_;
  CacheStats stats_;
};

}  // namespace perfsim
