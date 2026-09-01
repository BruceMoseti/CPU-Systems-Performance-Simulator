#include "config.hpp"

#include <algorithm>
#include <sstream>
#include <string_view>
#include <vector>

namespace perfsim {
namespace {

bool is_power_of_two(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

[[noreturn]] void fail(const std::string& message) { throw JsonError(message); }

// Reads a JSON object while remembering which keys were consumed, so that
// leftover keys can be reported as errors.
class ObjectReader {
 public:
  ObjectReader(const Json& object, std::string path)
      : object_(object), path_(std::move(path)) {
    if (!object_.is_object()) fail(path_ + ": expected a JSON object");
  }

  double number(std::string_view key, double fallback) {
    const Json* value = take(key);
    if (value == nullptr) return fallback;
    if (!value->is_number()) fail(where(key) + ": expected a number");
    return value->as_number();
  }

  uint32_t integer(std::string_view key, uint32_t fallback) {
    const double value = number(key, static_cast<double>(fallback));
    if (value < 0 || value != static_cast<double>(static_cast<uint32_t>(value))) {
      fail(where(key) + ": expected a non-negative integer");
    }
    return static_cast<uint32_t>(value);
  }

  const Json* child(std::string_view key) { return take(key); }

  void finish() const {
    for (const Json::Field& field : object_.fields()) {
      if (std::find(consumed_.begin(), consumed_.end(), field.first) == consumed_.end()) {
        fail(where(field.first) + ": unknown configuration key");
      }
    }
  }

 private:
  const Json* take(std::string_view key) {
    consumed_.emplace_back(key);
    return object_.find(key);
  }

  std::string where(std::string_view key) const { return path_ + "." + std::string(key); }

  const Json& object_;
  std::string path_;
  std::vector<std::string> consumed_;
};

CacheConfig read_cache(const Json* node, const CacheConfig& defaults, bool required) {
  CacheConfig cache = defaults;
  if (node == nullptr) {
    cache.enabled = required;
    return cache;
  }
  ObjectReader reader(*node, cache.name);
  cache.enabled = true;
  cache.size_kb = reader.integer("size_kb", cache.size_kb);
  cache.line_size = reader.integer("line_size", cache.line_size);
  cache.associativity = reader.integer("associativity", cache.associativity);
  cache.latency_cycles = reader.integer("latency_cycles", cache.latency_cycles);
  reader.finish();

  if (cache.size_kb == 0) fail(cache.name + ": size_kb must be greater than zero");
  if (!is_power_of_two(cache.line_size) || cache.line_size < 4) {
    fail(cache.name + ": line_size must be a power of two of at least 4 bytes");
  }
  if (cache.associativity == 0) fail(cache.name + ": associativity must be at least 1");
  if (cache.size_bytes() % cache.line_size != 0) {
    fail(cache.name + ": size must be a multiple of line_size");
  }
  if (cache.line_count() % cache.associativity != 0) {
    std::ostringstream oss;
    oss << cache.name << ": " << cache.line_count() << " lines cannot be divided into "
        << cache.associativity << "-way sets";
    fail(oss.str());
  }
  if (cache.latency_cycles == 0) fail(cache.name + ": latency_cycles must be at least 1");
  return cache;
}

}  // namespace

Config::Config() {
  l1.name = "l1";
  l1.size_kb = 32;
  l1.line_size = 64;
  l1.associativity = 8;
  l1.latency_cycles = 4;

  l2.name = "l2";
  l2.size_kb = 512;
  l2.line_size = 64;
  l2.associativity = 8;
  l2.latency_cycles = 12;
}

Config Config::from_json(const Json& root) {
  Config config;
  ObjectReader reader(root, "config");

  if (const Json* cpu = reader.child("cpu")) {
    ObjectReader cpu_reader(*cpu, "cpu");
    config.cpu.frequency_ghz = cpu_reader.number("frequency_ghz", config.cpu.frequency_ghz);
    config.cpu.issue_width = cpu_reader.integer("issue_width", config.cpu.issue_width);
    config.cpu.base_cpi = cpu_reader.number("base_cpi", config.cpu.base_cpi);
    config.cpu.mshrs = cpu_reader.integer("mshrs", config.cpu.mshrs);
    cpu_reader.finish();
    if (config.cpu.frequency_ghz <= 0) fail("cpu.frequency_ghz must be positive");
    if (config.cpu.issue_width == 0) fail("cpu.issue_width must be at least 1");
    if (config.cpu.base_cpi < 0) fail("cpu.base_cpi must not be negative");
    if (config.cpu.mshrs == 0) fail("cpu.mshrs must be at least 1");
  }

  config.l1 = read_cache(reader.child("l1"), config.l1, /*required=*/true);
  config.l2 = read_cache(reader.child("l2"), config.l2, /*required=*/false);

  if (const Json* memory = reader.child("memory")) {
    ObjectReader memory_reader(*memory, "memory");
    config.memory.latency_cycles =
        memory_reader.integer("latency_cycles", config.memory.latency_cycles);
    config.memory.bandwidth_gbps =
        memory_reader.number("bandwidth_gbps", config.memory.bandwidth_gbps);
    memory_reader.finish();
    if (config.memory.latency_cycles == 0) fail("memory.latency_cycles must be at least 1");
    if (config.memory.bandwidth_gbps <= 0) fail("memory.bandwidth_gbps must be positive");
  }

  reader.finish();
  return config;
}

Config Config::from_file(const std::string& path) { return from_json(Json::parse_file(path)); }

Json Config::to_json() const {
  Json cpu = Json::object();
  cpu.set("frequency_ghz", Json::number(this->cpu.frequency_ghz));
  cpu.set("issue_width", Json::number(this->cpu.issue_width));
  cpu.set("base_cpi", Json::number(this->cpu.base_cpi));
  cpu.set("mshrs", Json::number(this->cpu.mshrs));

  auto cache_to_json = [](const CacheConfig& cache) {
    Json node = Json::object();
    node.set("size_kb", Json::number(cache.size_kb));
    node.set("line_size", Json::number(cache.line_size));
    node.set("associativity", Json::number(cache.associativity));
    node.set("latency_cycles", Json::number(cache.latency_cycles));
    return node;
  };

  Json memory = Json::object();
  memory.set("latency_cycles", Json::number(this->memory.latency_cycles));
  memory.set("bandwidth_gbps", Json::number(this->memory.bandwidth_gbps));

  Json root = Json::object();
  root.set("cpu", std::move(cpu));
  root.set("l1", cache_to_json(l1));
  if (l2.enabled) root.set("l2", cache_to_json(l2));
  root.set("memory", std::move(memory));
  return root;
}

}  // namespace perfsim
