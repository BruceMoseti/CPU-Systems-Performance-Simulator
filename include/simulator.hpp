// Drives a trace through the modelled machine and derives reportable metrics.
#pragma once

#include <string>
#include <vector>

#include "config.hpp"
#include "cpu.hpp"
#include "json.hpp"
#include "memory.hpp"
#include "trace.hpp"

namespace perfsim {

struct Bottleneck {
  std::string name;
  std::string detail;
  std::vector<std::string> suggestions;
};

struct Results {
  std::string trace;
  Config config;
  CpuStats cpu;
  CacheStats l1;
  bool has_l2 = false;
  CacheStats l2;
  MemoryStats memory;

  double ipc = 0.0;
  double cpi = 0.0;
  double seconds = 0.0;
  double average_memory_latency = 0.0;
  // Average number of misses in flight; the direct measure of how well the
  // machine is hiding memory latency.
  double average_outstanding_misses = 0.0;
  double achieved_bandwidth_gbps = 0.0;
  double bandwidth_utilization = 0.0;
  double stall_fraction = 0.0;

  Bottleneck bottleneck;

  Json to_json() const;
};

class Simulator {
 public:
  explicit Simulator(const Config& config);

  void execute(const TraceRecord& record) { cpu_.execute(record); }
  Results finish();

  // Convenience wrapper: stream a whole trace and return the results.
  Results run(TraceReader& reader, const std::string& trace_name);

 private:
  Config config_;
  MemoryHierarchy hierarchy_;
  Cpu cpu_;
};

Bottleneck classify_bottleneck(const Results& results);

}  // namespace perfsim
