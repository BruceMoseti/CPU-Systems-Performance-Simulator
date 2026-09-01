#include "simulator.hpp"

#include <algorithm>
#include <array>

namespace perfsim {
namespace {

// A machine that spends less than this share of its cycles stalled is limited by
// how fast it can issue instructions, not by memory.
constexpr double kComputeBoundStallFraction = 0.25;
// Past this share of the configured bandwidth, queueing dominates and adding
// capacity or MSHRs cannot help.
constexpr double kBandwidthSaturated = 0.80;

double ratio(uint64_t numerator, uint64_t denominator) {
  return denominator == 0 ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator);
}

}  // namespace

Simulator::Simulator(const Config& config)
    : config_(config), hierarchy_(config_), cpu_(config_.cpu, hierarchy_) {}

Results Simulator::finish() {
  cpu_.drain();

  Results results;
  results.config = config_;
  results.cpu = cpu_.stats();
  results.l1 = hierarchy_.l1().stats();
  if (const Cache* l2 = hierarchy_.l2()) {
    results.has_l2 = true;
    results.l2 = l2->stats();
  }
  results.memory = hierarchy_.memory().stats();

  const CpuStats& cpu = results.cpu;
  results.ipc = ratio(cpu.instructions, cpu.cycles);
  results.cpi = cpu.instructions == 0
                    ? 0.0
                    : static_cast<double>(cpu.cycles) / static_cast<double>(cpu.instructions);
  results.seconds = static_cast<double>(cpu.cycles) / (config_.cpu.frequency_ghz * 1e9);
  results.average_memory_latency = ratio(cpu.access_latency_cycles, cpu.memory_instructions);
  results.average_outstanding_misses = ratio(cpu.miss_latency_cycles, cpu.cycles);
  results.stall_fraction = ratio(cpu.stalls.total(), cpu.cycles);

  const uint64_t dram_bytes = results.memory.bytes_read + results.memory.bytes_written;
  results.achieved_bandwidth_gbps =
      results.seconds > 0.0 ? static_cast<double>(dram_bytes) / results.seconds / 1e9 : 0.0;
  results.bandwidth_utilization =
      results.achieved_bandwidth_gbps / config_.memory.bandwidth_gbps;

  results.bottleneck = classify_bottleneck(results);
  return results;
}

Results Simulator::run(TraceReader& reader, const std::string& trace_name) {
  TraceRecord record;
  while (reader.next(record)) execute(record);
  Results results = finish();
  results.trace = trace_name;
  return results;
}

Bottleneck classify_bottleneck(const Results& results) {
  const StallCycles& stalls = results.cpu.stalls;
  Bottleneck bottleneck;

  if (results.stall_fraction < kComputeBoundStallFraction) {
    bottleneck.name = "Compute";
    bottleneck.detail = "Memory stalls account for only " +
                        std::to_string(static_cast<int>(results.stall_fraction * 100.0)) +
                        "% of cycles; the machine is limited by instruction issue.";
    bottleneck.suggestions = {"Increase cpu.issue_width",
                              "Reduce the instruction count (vectorise the inner loop)"};
    return bottleneck;
  }

  if (results.bandwidth_utilization >= kBandwidthSaturated) {
    bottleneck.name = "DRAM bandwidth";
    bottleneck.detail = "DRAM is delivering " +
                        std::to_string(static_cast<int>(results.bandwidth_utilization * 100.0)) +
                        "% of its configured bandwidth, so requests are queueing behind the bus.";
    bottleneck.suggestions = {"Increase memory.bandwidth_gbps",
                              "Reduce DRAM traffic by improving locality (blocking/tiling)",
                              "Increase last-level cache capacity to filter traffic"};
    return bottleneck;
  }

  const uint64_t dram_total = stalls.dram + stalls.bandwidth;
  const std::array<std::pair<uint64_t, const char*>, 4> candidates = {{
      {stalls.l1, "l1"},
      {stalls.l2, "l2"},
      {dram_total, "dram"},
      {stalls.mshr, "mshr"},
  }};
  const auto worst = std::max_element(candidates.begin(), candidates.end(),
                                      [](const auto& a, const auto& b) { return a.first < b.first; });
  const double share = ratio(worst->first, results.cpu.cycles);
  const std::string share_text = std::to_string(static_cast<int>(share * 100.0)) + "% of cycles";

  const std::string kind = worst->second;
  if (kind == "dram") {
    bottleneck.name = "DRAM latency";
    bottleneck.detail = "Waiting for DRAM costs " + share_text +
                        " and the bus is not saturated, so the exposed latency is the problem.";
    bottleneck.suggestions = {"Increase l2.size_kb to keep the working set on chip",
                              "Reduce memory.latency_cycles",
                              "Increase cpu.mshrs if the misses are independent"};
  } else if (kind == "mshr") {
    bottleneck.name = "Memory-level parallelism";
    bottleneck.detail = "Waiting for a free MSHR costs " + share_text +
                        ": the misses are independent but too few can be in flight at once.";
    bottleneck.suggestions = {"Increase cpu.mshrs",
                              "Reduce memory.latency_cycles to shorten MSHR occupancy",
                              "Improve locality so fewer misses need to be tracked"};
  } else if (kind == "l2") {
    bottleneck.name = "L1 capacity";
    bottleneck.detail = "L2 hits cost " + share_text +
                        ": the working set is escaping L1 but is still caught by L2.";
    bottleneck.suggestions = {"Increase l1.size_kb",
                              "Increase l1.associativity if the misses are conflicts",
                              "Reduce l2.latency_cycles"};
  } else {
    bottleneck.name = "Dependent load latency";
    bottleneck.detail = "Dependent loads that hit in L1 cost " + share_text +
                        ": the workload is serialised on its own load chain.";
    bottleneck.suggestions = {"Reduce l1.latency_cycles",
                              "Restructure the workload to break the dependence chain"};
  }
  return bottleneck;
}

Json Results::to_json() const {
  auto cache_json = [](const CacheStats& stats) {
    Json node = Json::object();
    node.set("accesses", Json::number(static_cast<double>(stats.accesses)));
    node.set("hits", Json::number(static_cast<double>(stats.hits)));
    node.set("misses", Json::number(static_cast<double>(stats.misses)));
    node.set("hit_rate", Json::number(stats.hit_rate()));
    node.set("writebacks", Json::number(static_cast<double>(stats.writebacks)));
    node.set("writebacks_in", Json::number(static_cast<double>(stats.writebacks_in)));
    return node;
  };

  Json stall = Json::object();
  stall.set("l1", Json::number(static_cast<double>(cpu.stalls.l1)));
  stall.set("l2", Json::number(static_cast<double>(cpu.stalls.l2)));
  stall.set("dram", Json::number(static_cast<double>(cpu.stalls.dram)));
  stall.set("bandwidth", Json::number(static_cast<double>(cpu.stalls.bandwidth)));
  stall.set("mshr", Json::number(static_cast<double>(cpu.stalls.mshr)));
  stall.set("total", Json::number(static_cast<double>(cpu.stalls.total())));

  Json dram = Json::object();
  dram.set("reads", Json::number(static_cast<double>(memory.reads)));
  dram.set("writes", Json::number(static_cast<double>(memory.writes)));
  dram.set("bytes_read", Json::number(static_cast<double>(memory.bytes_read)));
  dram.set("bytes_written", Json::number(static_cast<double>(memory.bytes_written)));
  dram.set("queue_delay_cycles", Json::number(static_cast<double>(memory.queue_delay_cycles)));

  Json suggestions = Json::array();
  for (const std::string& suggestion : bottleneck.suggestions) {
    suggestions.push_back(Json::string(suggestion));
  }
  Json bottleneck_json = Json::object();
  bottleneck_json.set("name", Json::string(bottleneck.name));
  bottleneck_json.set("detail", Json::string(bottleneck.detail));
  bottleneck_json.set("suggestions", std::move(suggestions));

  Json root = Json::object();
  root.set("trace", Json::string(trace));
  root.set("config", config.to_json());
  root.set("instructions", Json::number(static_cast<double>(cpu.instructions)));
  root.set("cycles", Json::number(static_cast<double>(cpu.cycles)));
  root.set("ipc", Json::number(ipc));
  root.set("cpi", Json::number(cpi));
  root.set("seconds", Json::number(seconds));
  root.set("memory_instructions", Json::number(static_cast<double>(cpu.memory_instructions)));
  root.set("dependent_loads", Json::number(static_cast<double>(cpu.dependent_loads)));
  root.set("l1", cache_json(l1));
  if (has_l2) root.set("l2", cache_json(l2));
  root.set("memory", std::move(dram));
  root.set("average_memory_latency", Json::number(average_memory_latency));
  root.set("average_outstanding_misses", Json::number(average_outstanding_misses));
  root.set("achieved_bandwidth_gbps", Json::number(achieved_bandwidth_gbps));
  root.set("bandwidth_utilization", Json::number(bandwidth_utilization));
  root.set("compute_cycles", Json::number(static_cast<double>(cpu.compute_cycles)));
  root.set("stall_cycles", std::move(stall));
  root.set("stall_fraction", Json::number(stall_fraction));
  root.set("bottleneck", std::move(bottleneck_json));
  return root;
}

}  // namespace perfsim
