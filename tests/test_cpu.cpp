#include "simulator.hpp"
#include "test_util.hpp"

using namespace perfsim;

namespace {

// A 1 GHz machine with the default cache geometry and effectively unlimited
// bandwidth, so every cycle count below can be derived by hand from the
// configured latencies alone.
Config make_machine(uint32_t issue_width, uint32_t mshrs) {
  Config config;
  config.cpu.frequency_ghz = 1.0;
  config.cpu.issue_width = issue_width;
  config.cpu.base_cpi = 0.0;
  config.cpu.mshrs = mshrs;
  config.memory.latency_cycles = 180;
  config.memory.bandwidth_gbps = 1e6;
  return config;
}

TraceRecord read(uint64_t address) { return {RecordType::Read, address}; }
TraceRecord dependent(uint64_t address) { return {RecordType::DependentRead, address}; }
TraceRecord compute(uint64_t count) { return {RecordType::Compute, count}; }

}  // namespace

TEST(issue_width_sets_the_compute_floor) {
  Simulator simulator(make_machine(/*issue_width=*/4, /*mshrs=*/16));
  simulator.execute(compute(8));
  const Results results = simulator.finish();

  CHECK_EQ(results.cpu.instructions, 8u);
  CHECK_EQ(results.cpu.cycles, 2u);
  CHECK_EQ(results.cpu.compute_cycles, 2u);
  CHECK_EQ(results.cpu.stalls.total(), 0u);
  CHECK_NEAR(results.ipc, 4.0, 1e-9);
}

// A dependent load that goes all the way to DRAM exposes L1 + L2 + DRAM latency:
// 4 + 12 + 180 = 196 cycles on top of the single issue cycle.
TEST(dependent_dram_load_exposes_full_latency) {
  Simulator simulator(make_machine(/*issue_width=*/1, /*mshrs=*/1));
  simulator.execute(dependent(0x1000));
  const Results results = simulator.finish();

  CHECK_EQ(results.cpu.cycles, 197u);
  CHECK_EQ(results.cpu.compute_cycles, 1u);
  CHECK_EQ(results.cpu.stalls.dram, 196u);
  CHECK_EQ(results.cpu.stalls.mshr, 0u);
  CHECK_NEAR(results.average_memory_latency, 196.0, 1e-9);
}

// The same four misses, with enough MSHRs to overlap them: four independent
// misses cost barely more than one.
TEST(independent_misses_overlap_up_to_the_mshr_limit) {
  Simulator simulator(make_machine(/*issue_width=*/1, /*mshrs=*/4));
  for (uint64_t line = 0; line < 4; ++line) simulator.execute(read(line * 64));
  const Results results = simulator.finish();

  CHECK_EQ(results.cpu.instructions, 4u);
  CHECK_EQ(results.cpu.cycles, 200u);
  CHECK_EQ(results.cpu.stalls.dram, 196u);
  CHECK_EQ(results.cpu.stalls.mshr, 0u);
  CHECK_NEAR(results.average_outstanding_misses, 4.0 * 196.0 / 200.0, 1e-9);
}

// One MSHR forces the same four misses to run one at a time, and the lost cycles
// are attributed to the parallelism limit rather than to DRAM.
TEST(single_mshr_serialises_independent_misses) {
  Simulator simulator(make_machine(/*issue_width=*/1, /*mshrs=*/1));
  for (uint64_t line = 0; line < 4; ++line) simulator.execute(read(line * 64));
  const Results results = simulator.finish();

  CHECK_EQ(results.cpu.cycles, 785u);
  CHECK_EQ(results.cpu.stalls.mshr, 585u);
  CHECK_EQ(results.cpu.stalls.dram, 196u);
  CHECK_EQ(results.cpu.compute_cycles, 4u);
  CHECK_EQ(results.cpu.stalls.total() + results.cpu.compute_cycles, results.cpu.cycles);
}

TEST(l1_hit_is_free_unless_a_dependent_load_needs_it) {
  Simulator independent_case(make_machine(/*issue_width=*/1, /*mshrs=*/1));
  independent_case.execute(dependent(0x1000));  // 197 cycles, warms the line
  independent_case.execute(read(0x1000));       // hits L1, fully pipelined
  const Results independent_results = independent_case.finish();
  CHECK_EQ(independent_results.cpu.cycles, 198u);
  CHECK_EQ(independent_results.cpu.stalls.l1, 0u);

  Simulator dependent_case(make_machine(/*issue_width=*/1, /*mshrs=*/1));
  dependent_case.execute(dependent(0x1000));
  dependent_case.execute(dependent(0x1000));  // hits L1 but blocks the pipeline
  const Results dependent_results = dependent_case.finish();
  CHECK_EQ(dependent_results.cpu.cycles, 202u);
  CHECK_EQ(dependent_results.cpu.stalls.l1, 4u);
  CHECK_NEAR(dependent_results.average_memory_latency, 100.0, 1e-9);
}

TEST(l2_hit_costs_l1_plus_l2_latency) {
  Config config = make_machine(/*issue_width=*/1, /*mshrs=*/4);
  // A 1 KB direct-mapped L1 makes it easy to evict a line from L1 while leaving
  // it resident in the much larger L2.
  config.l1.size_kb = 1;
  config.l1.associativity = 1;

  Simulator simulator(config);
  // Dependent warm-up accesses so that nothing is still in flight when the L2
  // hit happens, which keeps the cycle count exactly derivable.
  simulator.execute(dependent(0x0000));  // cold: fills L1 and L2, 196 cycles
  simulator.execute(dependent(0x0400));  // evicts line 0 from L1 only, 196 cycles
  simulator.execute(dependent(0x0000));  // must come back from L2
  const Results results = simulator.finish();

  CHECK_EQ(results.cpu.stalls.l2, 16u);     // 4 cycle L1 lookup + 12 cycle L2 hit
  CHECK_EQ(results.cpu.stalls.dram, 392u);  // the two cold misses only
  CHECK_EQ(results.cpu.cycles, 411u);
  CHECK_EQ(results.l2.hits, 1u);
  CHECK_EQ(results.l2.accesses, 3u);
}

TEST(disabling_l2_removes_it_from_the_latency_path) {
  Config config = make_machine(/*issue_width=*/1, /*mshrs=*/1);
  config.l2.enabled = false;

  Simulator simulator(config);
  simulator.execute(dependent(0x1000));
  const Results results = simulator.finish();

  CHECK(!results.has_l2);
  CHECK_EQ(results.cpu.stalls.dram, 184u);  // 4 cycle L1 lookup + 180 cycle DRAM
  CHECK_EQ(results.cpu.cycles, 185u);
}

// Independent misses never expose their own latency, so a bandwidth limit can
// only reach the CPU through MSHR occupancy. Those stalls must still be reported
// as a bandwidth problem rather than as a parallelism problem.
TEST(saturated_bandwidth_is_reported_as_bandwidth_not_mshr_pressure) {
  Config config = make_machine(/*issue_width=*/4, /*mshrs=*/16);
  // 1 byte per cycle at 1 GHz, so a 64-byte line occupies the bus for 64 cycles.
  config.memory.bandwidth_gbps = 1.0;

  Simulator simulator(config);
  for (uint64_t line = 0; line < 256; ++line) simulator.execute(read(line * 64));
  const Results results = simulator.finish();

  CHECK(results.memory.queue_delay_cycles > 0u);
  CHECK(results.bandwidth_utilization > 0.80);
  CHECK(results.cpu.stalls.bandwidth > results.cpu.stalls.mshr);
  CHECK_EQ(results.bottleneck.name, std::string("DRAM bandwidth"));
}

// A dependent chain cannot saturate the bus no matter how narrow it is: it only
// has one request outstanding at a time.
TEST(dependent_chain_is_latency_bound_not_bandwidth_bound) {
  Config config = make_machine(/*issue_width=*/4, /*mshrs=*/16);
  config.memory.bandwidth_gbps = 1.0;

  Simulator simulator(config);
  for (uint64_t line = 0; line < 256; ++line) simulator.execute(dependent(line * 64));
  const Results results = simulator.finish();

  CHECK(results.bandwidth_utilization < 0.40);
  CHECK(results.cpu.stalls.dram > results.cpu.stalls.bandwidth);
  CHECK_EQ(results.bottleneck.name, std::string("DRAM latency"));
}

TEST(stall_buckets_partition_every_cycle) {
  Simulator simulator(make_machine(/*issue_width=*/2, /*mshrs=*/2));
  uint64_t address = 0;
  for (int i = 0; i < 200; ++i) {
    simulator.execute(read(address));
    simulator.execute(dependent(address + 8));
    simulator.execute(compute(3));
    address += 64;
  }
  const Results results = simulator.finish();

  CHECK_EQ(results.cpu.compute_cycles + results.cpu.stalls.total(), results.cpu.cycles);
  CHECK_NEAR(
      results.stall_fraction,
      static_cast<double>(results.cpu.stalls.total()) / static_cast<double>(results.cpu.cycles),
      1e-12);
}
