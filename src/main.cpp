#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "config.hpp"
#include "report.hpp"
#include "simulator.hpp"
#include "trace.hpp"

namespace {

using namespace perfsim;

constexpr char kUsage[] =
    "perfsim - CPU and memory hierarchy performance simulator\n"
    "\n"
    "Usage:\n"
    "  perfsim --trace <path> [--config <path>] [--json <path>] [--quiet]\n"
    "  perfsim --benchmark [--accesses <n>] [--pattern <p>] [--config <path>]\n"
    "\n"
    "Options:\n"
    "  --config <path>   architecture description in JSON (default: built-in baseline)\n"
    "  --trace <path>    instruction/memory trace; \"-\" reads standard input\n"
    "  --json <path>     write machine-readable results; \"-\" writes to stdout\n"
    "  --quiet           suppress the human-readable report\n"
    "  --benchmark       measure simulator throughput instead of analysing a trace\n"
    "  --accesses <n>    accesses to simulate in benchmark mode (default 10000000)\n"
    "  --pattern <p>     benchmark pattern: sequential | random | pointer_chase\n"
    "  --help            show this message\n";

struct Options {
  std::string config_path;
  std::string trace_path;
  std::string json_path;
  bool quiet = false;
  bool benchmark = false;
  uint64_t accesses = 10'000'000;
  std::string pattern = "random";
};

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "perfsim: " << message << "\n";
  std::exit(1);
}

std::string require_value(int argc, char** argv, int& i) {
  if (i + 1 >= argc) fail(std::string("missing value for ") + argv[i]);
  return argv[++i];
}

Options parse_arguments(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config") {
      options.config_path = require_value(argc, argv, i);
    } else if (arg == "--trace") {
      options.trace_path = require_value(argc, argv, i);
    } else if (arg == "--json") {
      options.json_path = require_value(argc, argv, i);
    } else if (arg == "--quiet") {
      options.quiet = true;
    } else if (arg == "--benchmark") {
      options.benchmark = true;
    } else if (arg == "--accesses") {
      options.accesses = std::strtoull(require_value(argc, argv, i).c_str(), nullptr, 10);
    } else if (arg == "--pattern") {
      options.pattern = require_value(argc, argv, i);
    } else if (arg == "--help" || arg == "-h") {
      std::cout << kUsage;
      std::exit(0);
    } else {
      fail("unknown option " + arg + "\n\n" + kUsage);
    }
  }
  if (!options.benchmark && options.trace_path.empty()) {
    fail(std::string("either --trace or --benchmark is required\n\n") + kUsage);
  }
  return options;
}

void write_json(const std::string& path, const Json& value) {
  const std::string text = value.dump();
  if (path == "-") {
    std::cout << text;
    return;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) fail("cannot write JSON results to " + path);
  out << text;
}

// Feeds records straight into the simulator so that benchmark throughput
// measures the model itself rather than trace parsing.
uint64_t run_benchmark(Simulator& simulator, const Options& options) {
  const uint64_t stride = 64;
  uint64_t records = 0;
  uint64_t address = 0;
  uint64_t rng = 0x2545F4914F6CDD1DULL;
  TraceRecord record;

  const bool random = options.pattern == "random";
  const bool chase = options.pattern == "pointer_chase";
  if (!random && !chase && options.pattern != "sequential") {
    fail("unknown --pattern " + options.pattern);
  }

  for (uint64_t i = 0; i < options.accesses; ++i) {
    if (random || chase) {
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      address = (rng & 0x3FFFFFF) & ~UINT64_C(7);  // 64 MB footprint, 8-byte aligned
    } else {
      address += stride;
    }
    record.type = chase ? RecordType::DependentRead : RecordType::Read;
    record.value = address;
    simulator.execute(record);
    ++records;

    record.type = RecordType::Compute;
    record.value = 2;
    simulator.execute(record);
    ++records;
  }
  return records;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_arguments(argc, argv);

    Config config;
    if (!options.config_path.empty()) config = Config::from_file(options.config_path);

    Simulator simulator(config);
    const auto start = std::chrono::steady_clock::now();

    Results results;
    uint64_t records = 0;
    if (options.benchmark) {
      records = run_benchmark(simulator, options);
      results = simulator.finish();
      results.trace = "benchmark:" + options.pattern;
    } else {
      TraceReader reader(options.trace_path);
      TraceRecord record;
      while (reader.next(record)) {
        simulator.execute(record);
        ++records;
      }
      results = simulator.finish();
      results.trace = options.trace_path;
    }

    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double throughput = wall > 0.0 ? static_cast<double>(records) / wall : 0.0;

    if (!options.quiet) {
      std::cout << format_report(results);
      std::printf("\nSimulator: %.2f M records/s (%s records in %.2f s)\n", throughput / 1e6,
                  std::to_string(records).c_str(), wall);
    }

    if (!options.json_path.empty()) {
      Json root = results.to_json();
      Json simulator_json = Json::object();
      simulator_json.set("records", Json::number(static_cast<double>(records)));
      simulator_json.set("wall_seconds", Json::number(wall));
      simulator_json.set("records_per_second", Json::number(throughput));
      root.set("simulator", std::move(simulator_json));
      write_json(options.json_path, root);
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "perfsim: " << e.what() << "\n";
    return 1;
  }
}
