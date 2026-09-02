#include "cache.hpp"

#include <bit>

namespace perfsim {

Cache::Cache(const CacheConfig& config)
    : config_(config),
      line_shift_(static_cast<uint64_t>(std::countr_zero(config.line_size))),
      set_count_(config.set_count()),
      associativity_(config.associativity),
      tags_(config.line_count(), kEmptyTag),
      used_(config.line_count(), 0),
      dirty_(config.line_count(), 0) {
  if ((set_count_ & (set_count_ - 1)) == 0) set_mask_ = set_count_ - 1;
}

int Cache::find_way(size_t base, uint64_t line_address) const {
  const uint64_t* tags = tags_.data() + base;
  for (uint32_t way = 0; way < associativity_; ++way) {
    if (tags[way] == line_address) return static_cast<int>(way);
  }
  return -1;
}

int Cache::select_victim(size_t base) const {
  const uint64_t* used = used_.data() + base;
  int victim = 0;
  uint64_t oldest = used[0];
  for (uint32_t way = 1; way < associativity_; ++way) {
    if (used[way] < oldest) {
      oldest = used[way];
      victim = static_cast<int>(way);
    }
  }
  return victim;
}

CacheResult Cache::replace(size_t base, uint64_t line_address, bool dirty) {
  CacheResult result;
  const size_t victim = base + static_cast<size_t>(select_victim(base));
  if (tags_[victim] != kEmptyTag && dirty_[victim] != 0) {
    ++stats_.writebacks;
    result.dirty_eviction = true;
    result.evicted_address = tags_[victim] << line_shift_;
  }
  tags_[victim] = line_address;
  used_[victim] = tick_;
  dirty_[victim] = dirty ? 1 : 0;
  return result;
}

CacheResult Cache::access(uint64_t address, bool is_write) {
  const uint64_t line_address = address >> line_shift_;
  const size_t base = static_cast<size_t>(set_index(line_address)) * associativity_;

  ++stats_.accesses;
  ++tick_;

  const int way = find_way(base, line_address);
  if (way >= 0) {
    ++stats_.hits;
    used_[base + static_cast<size_t>(way)] = tick_;
    if (is_write) dirty_[base + static_cast<size_t>(way)] = 1;
    CacheResult result;
    result.hit = true;
    return result;
  }

  ++stats_.misses;
  return replace(base, line_address, is_write);
}

CacheResult Cache::install_writeback(uint64_t address) {
  const uint64_t line_address = address >> line_shift_;
  const size_t base = static_cast<size_t>(set_index(line_address)) * associativity_;

  ++stats_.writebacks_in;
  ++tick_;

  const int way = find_way(base, line_address);
  if (way >= 0) {
    used_[base + static_cast<size_t>(way)] = tick_;
    dirty_[base + static_cast<size_t>(way)] = 1;
    CacheResult result;
    result.hit = true;
    return result;
  }

  return replace(base, line_address, /*dirty=*/true);
}

}  // namespace perfsim
