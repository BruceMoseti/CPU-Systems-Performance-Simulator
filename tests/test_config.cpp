#include "config.hpp"
#include "test_util.hpp"

using namespace perfsim;

TEST(config_defaults_describe_a_plausible_desktop_core) {
  const Config config = Config::from_json(Json::parse("{}"));
  CHECK_NEAR(config.cpu.frequency_ghz, 3.5, 1e-12);
  CHECK_EQ(config.cpu.issue_width, 4u);
  CHECK_EQ(config.l1.size_kb, 32u);
  CHECK_EQ(config.l1.line_count(), 512u);
  CHECK_EQ(config.l1.set_count(), 64u);
  CHECK(config.l1.enabled);
  // An absent l2 section means the level is not present at all.
  CHECK(!config.l2.enabled);
}

TEST(config_reads_every_field) {
  const Config config = Config::from_json(Json::parse(R"({
    "cpu": {"frequency_ghz": 2.0, "issue_width": 2, "base_cpi": 0.8, "mshrs": 8},
    "l1": {"size_kb": 64, "line_size": 128, "associativity": 4, "latency_cycles": 5},
    "l2": {"size_kb": 1024, "line_size": 128, "associativity": 16, "latency_cycles": 20},
    "memory": {"latency_cycles": 250, "bandwidth_gbps": 12.5}
  })"));

  CHECK_NEAR(config.cpu.frequency_ghz, 2.0, 1e-12);
  CHECK_EQ(config.cpu.mshrs, 8u);
  CHECK_EQ(config.l1.line_size, 128u);
  CHECK_EQ(config.l2.size_kb, 1024u);
  CHECK(config.l2.enabled);
  CHECK_EQ(config.memory.latency_cycles, 250u);
  CHECK_NEAR(config.memory.bandwidth_gbps, 12.5, 1e-12);
  // base_cpi of 0.8 is worse than the 0.5 the issue width allows, so it wins.
  CHECK_NEAR(config.cpu.compute_cpi(), 0.8, 1e-12);
}

TEST(compute_cpi_falls_back_to_the_issue_width) {
  const Config config = Config::from_json(Json::parse(R"({"cpu": {"issue_width": 4}})"));
  CHECK_NEAR(config.cpu.compute_cpi(), 0.25, 1e-12);
}

// A mistyped sweep parameter would otherwise produce a whole set of results for
// the wrong machine.
TEST(config_rejects_unknown_keys) {
  CHECK_THROWS(Config::from_json(Json::parse(R"({"l1": {"size_mb": 1}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"cache": {"size_kb": 32}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"cpu": {"frequency": 3.5}})")));
}

TEST(config_rejects_impossible_geometry) {
  // Line size must be a power of two.
  CHECK_THROWS(Config::from_json(Json::parse(R"({"l1": {"line_size": 48}})")));
  // 32 KB of 64-byte lines is 512 lines, which cannot form 7-way sets.
  CHECK_THROWS(Config::from_json(Json::parse(R"({"l1": {"associativity": 7}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"l1": {"size_kb": 0}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"l1": {"latency_cycles": 0}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"cpu": {"issue_width": 0}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"cpu": {"mshrs": 0}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"memory": {"bandwidth_gbps": 0}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"cpu": {"issue_width": 2.5}})")));
}

// An out-of-range value has to be rejected while it is still a double.
// Converting one that does not fit the destination integer type is undefined
// behaviour, so validating after the conversion would be validating a value the
// compiler was free to invent.
TEST(config_rejects_numbers_that_do_not_fit) {
  CHECK_THROWS(Config::from_json(Json::parse(R"({"l1": {"size_kb": 1e400}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"l1": {"size_kb": 1e300}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"l1": {"size_kb": 5000000000}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"l1": {"size_kb": -1}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"cpu": {"mshrs": 1e300}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"memory": {"latency_cycles": 1e400}})")));
  // Infinities pass a bare positivity test, then silently poison the results:
  // an infinite clock gives a zero runtime and an infinite achieved bandwidth.
  CHECK_THROWS(Config::from_json(Json::parse(R"({"cpu": {"frequency_ghz": 1e400}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"cpu": {"base_cpi": 1e400}})")));
  CHECK_THROWS(Config::from_json(Json::parse(R"({"memory": {"bandwidth_gbps": 1e400}})")));
  // base_cpi is bounded so that Cpu::issue can convert its cycle debt without a
  // range check on the hot path.
  CHECK_THROWS(Config::from_json(Json::parse(R"({"cpu": {"base_cpi": 1e9}})")));
}

TEST(config_survives_a_json_round_trip) {
  const Config original = Config::from_json(Json::parse(R"({
    "cpu": {"frequency_ghz": 3.0, "issue_width": 6, "mshrs": 12},
    "l1": {"size_kb": 48, "associativity": 12},
    "l2": {"size_kb": 2048, "associativity": 16, "latency_cycles": 30},
    "memory": {"latency_cycles": 120, "bandwidth_gbps": 80}
  })"));
  const Config restored = Config::from_json(original.to_json());

  CHECK_EQ(restored.l1.size_kb, 48u);
  CHECK_EQ(restored.l1.associativity, 12u);
  CHECK_EQ(restored.l2.size_kb, 2048u);
  CHECK_EQ(restored.l2.latency_cycles, 30u);
  CHECK_EQ(restored.cpu.mshrs, 12u);
  CHECK_NEAR(restored.memory.bandwidth_gbps, 80.0, 1e-12);
}
