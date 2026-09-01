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
  uint64_t writebacks = 0;        // dirty lines sent to the next level
  uint64_t writebacks_in = 0;     // dirty lines received from the level above

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
  struct Line {
    uint64_t tag = 0;
    uint64_t last_used = 0;
    bool valid = false;
    bool dirty = false;
  };

  uint64_t set_index(uint64_t line_address) const {
    return set_mask_ != 0 ? (line_address & set_mask_) : line_address % set_count_;
  }

  // Returns the way holding line_address within its set, or -1 when absent.
  int find_way(uint64_t set, uint64_t line_address) const;
  // Chooses an invalid way, or the least recently used one when the set is full.
  int select_victim(uint64_t set) const;

  CacheConfig config_;
  uint64_t line_shift_ = 0;
  uint64_t set_count_ = 0;
  uint64_t set_mask_ = 0;  // non-zero only when set_count_ is a power of two
  uint32_t associativity_ = 1;
  uint64_t tick_ = 0;
  std::vector<Line> lines_;
  CacheStats stats_;
};

}  // namespace perfsim
