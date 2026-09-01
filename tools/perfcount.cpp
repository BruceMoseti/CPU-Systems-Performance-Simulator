// Minimal `perf stat` replacement built on perf_event_open.
//
// Phase 14 of the project compares the simulated model against real hardware.
// Depending on the `perf` binary is awkward because it is tied to the running
// kernel version and is frequently absent from containers, so this reads the
// counters directly and reports which ones the host actually exposes. On a VM
// without a virtualised PMU every hardware event fails to open; the tool then
// still returns wall-clock time, which is enough to compare predicted against
// measured runtime.
#include <asm/unistd.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "json.hpp"

namespace {

using perfsim::Json;

struct Counter {
  Counter(const char* counter_name, uint32_t event_type, uint64_t event_config)
      : name(counter_name), type(event_type), config(event_config) {}

  const char* name;
  uint32_t type;
  uint64_t config;
  int fd = -1;
  std::string error;
};

long perf_event_open(perf_event_attr* attr, pid_t pid, int cpu, int group_fd, unsigned long flags) {
  return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

std::vector<Counter> make_counters() {
  return {
      {"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
      {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
      {"cache_references", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES},
      {"cache_misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
      {"l1d_read_misses", PERF_TYPE_HW_CACHE,
       PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
           (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)},
      {"task_clock", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK},
  };
}

constexpr char kUsage[] =
    "perfcount - measure hardware counters for a command\n"
    "\n"
    "Usage:\n"
    "  perfcount -- <command> [args...]\n"
    "\n"
    "Prints a JSON object with the counters the host exposes plus wall-clock\n"
    "time. Hardware counters are unavailable inside most virtual machines; the\n"
    "\"unavailable\" field explains why when that happens.\n";

}  // namespace

int main(int argc, char** argv) {
  int command_start = 1;
  if (argc > 1 && std::string(argv[1]) == "--") command_start = 2;
  if (command_start >= argc) {
    std::cerr << kUsage;
    return 1;
  }

  std::string command_line;
  std::vector<char*> command;
  for (int i = command_start; i < argc; ++i) {
    command.push_back(argv[i]);
    if (!command_line.empty()) command_line += " ";
    command_line += argv[i];
  }
  command.push_back(nullptr);

  // The child waits on this pipe so that counters are armed before it execs.
  int ready_pipe[2];
  if (pipe(ready_pipe) != 0) {
    std::cerr << "perfcount: pipe failed: " << std::strerror(errno) << "\n";
    return 1;
  }

  const auto start = std::chrono::steady_clock::now();
  const pid_t child = fork();
  if (child < 0) {
    std::cerr << "perfcount: fork failed: " << std::strerror(errno) << "\n";
    return 1;
  }

  if (child == 0) {
    close(ready_pipe[1]);
    char signal = 0;
    while (read(ready_pipe[0], &signal, 1) < 0 && errno == EINTR) {
    }
    close(ready_pipe[0]);
    execvp(command[0], command.data());
    std::cerr << "perfcount: cannot execute " << command[0] << ": " << std::strerror(errno) << "\n";
    _exit(127);
  }

  close(ready_pipe[0]);
  std::vector<Counter> counters = make_counters();
  for (Counter& counter : counters) {
    perf_event_attr attr{};
    attr.size = sizeof(attr);
    attr.type = counter.type;
    attr.config = counter.config;
    attr.disabled = 1;
    attr.enable_on_exec = 1;
    attr.inherit = 1;
    // Counting kernel or hypervisor events requires privileges this tool should
    // not need, and user-space counts are what the simulator models anyway.
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    counter.fd = static_cast<int>(perf_event_open(&attr, child, -1, -1, 0));
    if (counter.fd < 0) counter.error = std::strerror(errno);
  }

  char go = 1;
  if (write(ready_pipe[1], &go, 1) != 1) {
    std::cerr << "perfcount: failed to release the child process\n";
  }
  close(ready_pipe[1]);

  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  const double wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  Json values = Json::object();
  Json unavailable = Json::object();
  for (Counter& counter : counters) {
    if (counter.fd < 0) {
      unavailable.set(counter.name, Json::string(counter.error));
      continue;
    }
    ioctl(counter.fd, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t value = 0;
    if (read(counter.fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))) {
      values.set(counter.name, Json::number(static_cast<double>(value)));
    } else {
      unavailable.set(counter.name, Json::string("counter could not be read"));
    }
    close(counter.fd);
  }

  const bool hardware_available = values.find("cycles") != nullptr &&
                                  values.find("instructions") != nullptr;

  Json root = Json::object();
  root.set("command", Json::string(command_line));
  root.set("exit_code", Json::number(WIFEXITED(status) ? WEXITSTATUS(status) : -1));
  root.set("wall_seconds", Json::number(wall));
  root.set("hardware_counters_available", Json::boolean(hardware_available));
  if (hardware_available) {
    const double cycles = values.find("cycles")->as_number();
    const double instructions = values.find("instructions")->as_number();
    if (cycles > 0) root.set("ipc", Json::number(instructions / cycles));
  }
  root.set("counters", std::move(values));
  root.set("unavailable", std::move(unavailable));
  std::cout << root.dump();
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
