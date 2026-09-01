#include "cache.hpp"

#include <bit>

namespace perfsim {

Cache::Cache(const CacheConfig& config)
    : config_(config),
      line_shift_(static_cast<uint64_t>(std::countr_zero(config.line_size))),
      set_count_(config.set_count()),
      associativity_(config.associativity),
      lines_(config.line_count()) {
  if ((set_count_ & (set_count_ - 1)) == 0) set_mask_ = set_count_ - 1;
}

int Cache::find_way(uint64_t set, uint64_t line_address) const {
  const size_t base = static_cast<size_t>(set) * associativity_;
  for (uint32_t way = 0; way < associativity_; ++way) {
    const Line& line = lines_[base + way];
    if (line.valid && line.tag == line_address) return static_cast<int>(way);
  }
  return -1;
}

int Cache::select_victim(uint64_t set) const {
  const size_t base = static_cast<size_t>(set) * associativity_;
  int victim = 0;
  uint64_t oldest = UINT64_MAX;
  for (uint32_t way = 0; way < associativity_; ++way) {
    const Line& line = lines_[base + way];
    if (!line.valid) return static_cast<int>(way);
    if (line.last_used < oldest) {
      oldest = line.last_used;
      victim = static_cast<int>(way);
    }
  }
  return victim;
}

CacheResult Cache::access(uint64_t address, bool is_write) {
  const uint64_t line_address = address >> line_shift_;
  const uint64_t set = set_index(line_address);
  const size_t base = static_cast<size_t>(set) * associativity_;

  ++stats_.accesses;
  ++tick_;

  CacheResult result;
  const int way = find_way(set, line_address);
  if (way >= 0) {
    ++stats_.hits;
    Line& line = lines_[base + way];
    line.last_used = tick_;
    if (is_write) line.dirty = true;
    result.hit = true;
    return result;
  }

  ++stats_.misses;
  const int victim = select_victim(set);
  Line& line = lines_[base + victim];
  if (line.valid && line.dirty) {
    ++stats_.writebacks;
    result.dirty_eviction = true;
    result.evicted_address = line.tag << line_shift_;
  }
  line.valid = true;
  line.dirty = is_write;
  line.tag = line_address;
  line.last_used = tick_;
  return result;
}

CacheResult Cache::install_writeback(uint64_t address) {
  const uint64_t line_address = address >> line_shift_;
  const uint64_t set = set_index(line_address);
  const size_t base = static_cast<size_t>(set) * associativity_;

  ++stats_.writebacks_in;
  ++tick_;

  CacheResult result;
  const int way = find_way(set, line_address);
  if (way >= 0) {
    Line& line = lines_[base + way];
    line.last_used = tick_;
    line.dirty = true;
    result.hit = true;
    return result;
  }

  const int victim = select_victim(set);
  Line& line = lines_[base + victim];
  if (line.valid && line.dirty) {
    ++stats_.writebacks;
    result.dirty_eviction = true;
    result.evicted_address = line.tag << line_shift_;
  }
  line.valid = true;
  line.dirty = true;
  line.tag = line_address;
  line.last_used = tick_;
  return result;
}

}  // namespace perfsim
