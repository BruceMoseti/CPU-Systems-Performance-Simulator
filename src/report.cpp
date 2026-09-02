#include "report.hpp"

#include <cstdarg>
#include <cstdio>
#include <sstream>

namespace perfsim {
namespace {

std::string with_commas(uint64_t value) {
  std::string digits = std::to_string(value);
  std::string out;
  const size_t leading = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i == leading && i != 0) out.push_back(',');
    if (i > leading && (i - leading) % 3 == 0) out.push_back(',');
    out.push_back(digits[i]);
  }
  return out;
}

__attribute__((format(printf, 1, 2))) std::string sprintf_str(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  return buffer;
}

std::string format_bytes(uint64_t bytes) {
  const double kb = static_cast<double>(bytes) / 1024.0;
  if (kb < 1024.0) return sprintf_str("%.1f KB", kb);
  const double mb = kb / 1024.0;
  if (mb < 1024.0) return sprintf_str("%.1f MB", mb);
  return sprintf_str("%.2f GB", mb / 1024.0);
}

std::string format_time(double seconds) {
  if (seconds < 1e-6) return sprintf_str("%.1f ns", seconds * 1e9);
  if (seconds < 1e-3) return sprintf_str("%.2f us", seconds * 1e6);
  if (seconds < 1.0) return sprintf_str("%.3f ms", seconds * 1e3);
  return sprintf_str("%.3f s", seconds);
}

std::string bar(double fraction, int width = 34) {
  int filled = static_cast<int>(fraction * width + 0.5);
  if (filled > width) filled = width;
  if (filled < 0) filled = 0;
  return std::string(static_cast<size_t>(filled), '#');
}

void stall_row(std::ostringstream& out, const char* label, uint64_t cycles, uint64_t total) {
  const double fraction =
      total == 0 ? 0.0 : static_cast<double>(cycles) / static_cast<double>(total);
  out << sprintf_str("  %-26s %5.1f%%  %-12s %s\n", label, fraction * 100.0,
                     with_commas(cycles).c_str(), bar(fraction).c_str());
}

std::string cache_line(const CacheConfig& cache) {
  return sprintf_str("%u KB, %u-way, %u B line, %u cycles", cache.size_kb, cache.associativity,
                     cache.line_size, cache.latency_cycles);
}

}  // namespace

std::string format_report(const Results& results) {
  const Config& config = results.config;
  const CpuStats& cpu = results.cpu;
  std::ostringstream out;

  out << "PerfSim Architecture Analysis\n";
  out << "=============================\n\n";

  if (!results.trace.empty()) out << "Workload:  " << results.trace << "\n";
  out << sprintf_str("CPU:       %.2f GHz, %u-wide issue, %u MSHRs, compute CPI %.2f\n",
                     config.cpu.frequency_ghz, config.cpu.issue_width, config.cpu.mshrs,
                     config.cpu.compute_cpi());
  out << "L1:        " << cache_line(config.l1) << "\n";
  if (config.l2.enabled) {
    out << "L2:        " << cache_line(config.l2) << "\n";
  } else {
    out << "L2:        disabled\n";
  }
  out << sprintf_str("DRAM:      %u cycles, %.1f GB/s\n", config.memory.latency_cycles,
                     config.memory.bandwidth_gbps);

  out << "\nPerformance\n-----------\n";
  out << sprintf_str("  %-26s %14s\n", "Instructions:", with_commas(cpu.instructions).c_str());
  out << sprintf_str("  %-26s %14s\n", "Cycles:", with_commas(cpu.cycles).c_str());
  // Three decimals because a latency-bound workload can sit below 0.02 IPC,
  // where two decimals hide every difference worth seeing.
  out << sprintf_str("  %-26s %14.3f\n", "IPC:", results.ipc);
  out << sprintf_str("  %-26s %14.2f\n", "CPI:", results.cpi);
  out << sprintf_str("  %-26s %14s\n", "Execution time:", format_time(results.seconds).c_str());

  out << "\nMemory hierarchy\n----------------\n";
  out << sprintf_str("  %-26s %14s   hit rate %5.1f%%\n",
                     "L1 accesses:", with_commas(results.l1.accesses).c_str(),
                     results.l1.hit_rate() * 100.0);
  if (results.has_l2) {
    out << sprintf_str("  %-26s %14s   hit rate %5.1f%%\n",
                       "L2 accesses:", with_commas(results.l2.accesses).c_str(),
                       results.l2.hit_rate() * 100.0);
  }
  out << sprintf_str("  %-26s %14s\n", "DRAM accesses:",
                     with_commas(results.memory.reads + results.memory.writes).c_str());
  out << sprintf_str("  %-26s %14.1f cycles\n",
                     "Average memory latency:", results.average_memory_latency);
  out << sprintf_str("  %-26s %14.2f\n",
                     "Avg outstanding misses:", results.average_outstanding_misses);
  out << sprintf_str("  %-26s %14s   %.1f GB/s (%.1f%% of %.1f GB/s)\n", "DRAM traffic:",
                     format_bytes(results.memory.bytes_read + results.memory.bytes_written).c_str(),
                     results.achieved_bandwidth_gbps, results.bandwidth_utilization * 100.0,
                     config.memory.bandwidth_gbps);

  out << "\nStall breakdown (share of total cycles)\n";
  out << "---------------------------------------\n";
  stall_row(out, "Compute", cpu.compute_cycles, cpu.cycles);
  stall_row(out, "L1 (dependent loads)", cpu.stalls.l1, cpu.cycles);
  stall_row(out, "L2", cpu.stalls.l2, cpu.cycles);
  stall_row(out, "DRAM latency", cpu.stalls.dram, cpu.cycles);
  stall_row(out, "DRAM bandwidth", cpu.stalls.bandwidth, cpu.cycles);
  stall_row(out, "MSHR (none free)", cpu.stalls.mshr, cpu.cycles);

  out << "\nDiagnosis\n---------\n";
  out << "  Primary bottleneck: " << results.bottleneck.name << "\n";
  out << "  " << results.bottleneck.detail << "\n";
  if (!results.bottleneck.suggestions.empty()) {
    out << "  Suggested changes:\n";
    for (size_t i = 0; i < results.bottleneck.suggestions.size(); ++i) {
      out << "    " << (i + 1) << ". " << results.bottleneck.suggestions[i] << "\n";
    }
  }
  return out.str();
}

}  // namespace perfsim
