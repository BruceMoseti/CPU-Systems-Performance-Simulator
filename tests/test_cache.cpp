#include "cache.hpp"
#include "test_util.hpp"

using namespace perfsim;

namespace {

CacheConfig make_cache(uint32_t size_kb, uint32_t line_size, uint32_t associativity) {
  CacheConfig config;
  config.name = "test";
  config.size_kb = size_kb;
  config.line_size = line_size;
  config.associativity = associativity;
  config.latency_cycles = 1;
  return config;
}

}  // namespace

// 1 KB with 64-byte lines, direct mapped: 16 sets, small enough to check the
// cold-miss and same-line cases by hand.
TEST(direct_mapped_hits_misses_and_conflicts) {
  Cache cache(make_cache(1, 64, 1));

  CHECK(!cache.access(0x0000, false).hit);  // cold miss
  CHECK(cache.access(0x0004, false).hit);   // same line
  CHECK(cache.access(0x0020, false).hit);   // same line, higher offset
  CHECK(!cache.access(0x0040, false).hit);  // next line, cold miss
  CHECK(cache.access(0x0000, false).hit);   // still resident

  const CacheStats& stats = cache.stats();
  CHECK_EQ(stats.accesses, 5u);
  CHECK_EQ(stats.hits, 3u);
  CHECK_EQ(stats.misses, 2u);
}

// 1 KB, 64-byte lines, direct mapped gives 16 sets, so lines 0 and 16 collide.
TEST(direct_mapped_conflict_miss) {
  Cache cache(make_cache(1, 64, 1));

  CHECK(!cache.access(0x000, false).hit);
  CHECK(cache.access(0x000, false).hit);
  CHECK(!cache.access(0x400, false).hit);  // line 16 -> set 0, evicts line 0
  CHECK(!cache.access(0x000, false).hit);  // conflict miss
  CHECK_EQ(cache.stats().hits, 1u);
  CHECK_EQ(cache.stats().misses, 3u);
}

// The victim must be the least recently used line, not the oldest inserted one.
TEST(lru_evicts_least_recently_used_not_first_inserted) {
  // 1 KB, 64-byte lines, 2-way: 8 sets. Lines 0, 8 and 16 all map to set 0.
  Cache cache(make_cache(1, 64, 2));

  const uint64_t a = 0x000;  // line 0
  const uint64_t b = 0x200;  // line 8
  const uint64_t c = 0x400;  // line 16

  CHECK(!cache.access(a, false).hit);
  CHECK(!cache.access(b, false).hit);
  CHECK(cache.access(a, false).hit);   // a becomes most recently used
  CHECK(!cache.access(c, false).hit);  // evicts b under LRU, a under FIFO
  CHECK(cache.access(a, false).hit);   // proves a survived
  CHECK(!cache.access(b, false).hit);  // proves b was the victim

  CHECK_EQ(cache.stats().hits, 2u);
  CHECK_EQ(cache.stats().misses, 4u);
}

// The same access pattern thrashes a direct-mapped cache and fits a fully
// associative one of identical capacity: conflict misses in isolation.
TEST(associativity_removes_conflict_misses) {
  const uint64_t addresses[] = {0x000, 0x400, 0x800, 0xC00};

  Cache direct_mapped(make_cache(1, 64, 1));
  Cache fully_associative(make_cache(1, 64, 16));

  for (int pass = 0; pass < 3; ++pass) {
    for (uint64_t address : addresses) {
      direct_mapped.access(address, false);
      fully_associative.access(address, false);
    }
  }

  // All four addresses map to set 0 when direct mapped, so nothing ever survives.
  CHECK_EQ(direct_mapped.stats().hits, 0u);
  CHECK_EQ(direct_mapped.stats().misses, 12u);

  // One set of 16 ways holds all four lines: only the cold misses remain.
  CHECK_EQ(fully_associative.stats().misses, 4u);
  CHECK_EQ(fully_associative.stats().hits, 8u);
}

TEST(dirty_line_is_written_back_on_eviction) {
  Cache cache(make_cache(1, 64, 1));

  CHECK(!cache.access(0x000, /*is_write=*/true).hit);
  CHECK_EQ(cache.stats().writebacks, 0u);

  const CacheResult evicting = cache.access(0x400, /*is_write=*/false);
  CHECK(!evicting.hit);
  CHECK(evicting.dirty_eviction);
  CHECK_EQ(evicting.evicted_address, 0x000u);
  CHECK_EQ(cache.stats().writebacks, 1u);

  // Evicting the clean replacement produces no further writeback.
  cache.access(0x000, false);
  CHECK_EQ(cache.stats().writebacks, 1u);
}

TEST(read_miss_then_eviction_is_not_a_writeback) {
  Cache cache(make_cache(1, 64, 1));
  cache.access(0x000, false);
  const CacheResult evicting = cache.access(0x400, false);
  CHECK(!evicting.dirty_eviction);
  CHECK_EQ(cache.stats().writebacks, 0u);
}

// Writebacks arriving from the level above must not move the hit rate, which
// should describe the workload's locality and nothing else.
TEST(installed_writeback_stays_out_of_hit_statistics) {
  Cache cache(make_cache(1, 64, 2));

  cache.install_writeback(0x000);
  CHECK_EQ(cache.stats().accesses, 0u);
  CHECK_EQ(cache.stats().hits, 0u);
  CHECK_EQ(cache.stats().writebacks_in, 1u);

  // The line is present, so a later demand access hits.
  CHECK(cache.access(0x000, false).hit);
  CHECK_EQ(cache.stats().accesses, 1u);

  // It is also dirty, so evicting it costs a writeback.
  cache.access(0x200, false);
  cache.access(0x400, false);
  CHECK_EQ(cache.stats().writebacks, 1u);
}

TEST(larger_line_captures_more_spatial_locality) {
  Cache small(make_cache(1, 16, 1));
  Cache large(make_cache(1, 64, 1));

  // Stride-8 walk over 1 KB: a 16-byte line covers two accesses, a 64-byte line
  // covers eight.
  for (uint64_t address = 0; address < 1024; address += 8) {
    small.access(address, false);
    large.access(address, false);
  }
  CHECK_EQ(small.stats().misses, 64u);
  CHECK_EQ(large.stats().misses, 16u);
}

TEST(non_power_of_two_set_count_still_indexes_correctly) {
  // 3 KB with 64-byte lines and 4 ways gives 12 sets, which is not a power of
  // two, so indexing must fall back to a modulo.
  Cache cache(make_cache(3, 64, 4));

  // Line 0 and line 12 share set 0; four ways hold both plus more.
  CHECK(!cache.access(0x000, false).hit);
  CHECK(!cache.access(0x300, false).hit);
  CHECK(cache.access(0x000, false).hit);
  CHECK(cache.access(0x300, false).hit);
  CHECK_EQ(cache.stats().hits, 2u);
}
