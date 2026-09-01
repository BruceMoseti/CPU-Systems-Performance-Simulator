#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "json.hpp"
#include "workloads.hpp"

namespace {

using namespace perfsim;
using namespace perfsim::workloads;

constexpr char kUsage[] =
    "tracegen - generate perfsim traces, or run the same kernel natively\n"
    "\n"
    "Usage:\n"
    "  tracegen <workload> [--mode trace|native] [options]\n"
    "  tracegen --list\n"
    "\n"
    "Options:\n"
    "  --mode <m>        trace (default) writes a trace; native runs the real kernel\n"
    "  --n <n>           elements, or matrix dimension for the matrix kernel\n"
    "  --block <b>       matrix blocking factor; omit or 0 for the unblocked version\n"
    "  --stride <s>      byte spacing between elements for the strided kernel\n"
    "  --iterations <i>  repeat the kernel (default 1)\n"
    "  --seed <s>        random seed (default 12345)\n"
    "  --out <path>      trace destination; \"-\" writes to stdout (default)\n"
    "  --help            show this message\n";

[[noreturn]] void fail(const std::string& message) {
  std::cerr << "tracegen: " << message << "\n";
  std::exit(1);
}

std::string require_value(int argc, char** argv, int& i) {
  if (i + 1 >= argc) fail(std::string("missing value for ") + argv[i]);
  return argv[++i];
}

uint64_t parse_u64(const std::string& text) {
  return std::strtoull(text.c_str(), nullptr, 10);
}

void print_list() {
  for (const Workload& workload : registry()) {
    std::printf("  %-16s %s\n", workload.name, workload.description);
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::string name;
    std::string mode = "trace";
    std::string out_path = "-";
    Params params;
    bool n_given = false;

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--list") {
        print_list();
        return 0;
      } else if (arg == "--help" || arg == "-h") {
        std::cout << kUsage << "\nWorkloads:\n";
        print_list();
        return 0;
      } else if (arg == "--mode") {
        mode = require_value(argc, argv, i);
      } else if (arg == "--n") {
        params.n = parse_u64(require_value(argc, argv, i));
        n_given = true;
      } else if (arg == "--block") {
        params.block = parse_u64(require_value(argc, argv, i));
      } else if (arg == "--stride") {
        params.stride = parse_u64(require_value(argc, argv, i));
      } else if (arg == "--iterations") {
        params.iterations = parse_u64(require_value(argc, argv, i));
      } else if (arg == "--seed") {
        params.seed = parse_u64(require_value(argc, argv, i));
      } else if (arg == "--out") {
        out_path = require_value(argc, argv, i);
      } else if (!arg.empty() && arg[0] == '-') {
        fail("unknown option " + arg + "\n\n" + kUsage);
      } else if (name.empty()) {
        name = arg;
      } else {
        fail("unexpected argument " + arg);
      }
    }

    if (name.empty()) fail("a workload name is required\n\n" + std::string(kUsage));
    const Workload* workload = find(name);
    if (workload == nullptr) fail("unknown workload " + name + " (try --list)");
    if (params.n == 0) fail("--n must be greater than zero");
    if (params.iterations == 0) fail("--iterations must be greater than zero");
    if (params.stride == 0 || params.stride % kElementSize != 0) {
      fail("--stride must be a non-zero multiple of " + std::to_string(kElementSize));
    }
    if (!n_given && std::string(workload->name) == "matrix") params.n = 128;

    if (mode == "trace") {
      const std::string header = "workload: " + std::string(workload->name) +
                                 " n=" + std::to_string(params.n) +
                                 " block=" + std::to_string(params.block) +
                                 " stride=" + std::to_string(params.stride) +
                                 " iterations=" + std::to_string(params.iterations) +
                                 " seed=" + std::to_string(params.seed);
      TraceWriter writer(out_path, header);
      workload->emit(writer, params);
      writer.close();
      return 0;
    }

    if (mode != "native") fail("unknown --mode " + mode);

    const NativeResult native = workload->native(params);

    Json result = Json::object();
    result.set("workload", Json::string(workload->name));
    result.set("mode", Json::string("native"));
    result.set("n", Json::number(static_cast<double>(params.n)));
    result.set("block", Json::number(static_cast<double>(params.block)));
    result.set("stride", Json::number(static_cast<double>(params.stride)));
    result.set("iterations", Json::number(static_cast<double>(params.iterations)));
    result.set("seed", Json::number(static_cast<double>(params.seed)));
    result.set("seconds", Json::number(native.seconds));
    result.set("checksum", Json::string(std::to_string(native.checksum)));
    std::cout << result.dump();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "tracegen: " << e.what() << "\n";
    return 1;
  }
}

namespace perfsim::workloads {

const std::vector<Workload>& registry() {
  static const std::vector<Workload> workloads = {
      {"sequential", "stride-1 scan of an array (excellent spatial locality)", emit_sequential,
       native_sequential},
      {"random_access", "independent random loads (poor locality, misses overlap)",
       emit_random_access, native_random_access},
      {"pointer_chase", "dependent loads following a random cycle (latency exposed)",
       emit_pointer_chase, native_pointer_chase},
      {"strided", "random loads into a strided array of structs (conflict misses)", emit_strided,
       native_strided},
      {"matrix", "matrix multiply; --block turns the naive nest into a tiled one", emit_matrix,
       native_matrix},
  };
  return workloads;
}

const Workload* find(const std::string& name) {
  for (const Workload& workload : registry()) {
    if (name == workload.name) return &workload;
  }
  return nullptr;
}

}  // namespace perfsim::workloads
