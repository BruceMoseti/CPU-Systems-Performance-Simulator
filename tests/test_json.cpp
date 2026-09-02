#include <limits>

#include "json.hpp"
#include "test_util.hpp"

using namespace perfsim;

TEST(json_parses_a_nested_document) {
  const Json root = Json::parse(R"({
    "cpu": {"frequency_ghz": 3.5, "issue_width": 4},
    "names": ["l1", "l2"],
    "enabled": true,
    "missing": null,
    "negative": -12.5,
    "exponent": 1e3
  })");

  const Json* cpu = root.find("cpu");
  CHECK(cpu != nullptr);
  CHECK_NEAR(cpu->find("frequency_ghz")->as_number(), 3.5, 1e-12);
  CHECK_NEAR(cpu->find("issue_width")->as_number(), 4.0, 1e-12);
  CHECK_EQ(root.find("names")->elements().size(), 2u);
  CHECK_EQ(root.find("names")->elements()[1].as_string(), std::string("l2"));
  CHECK(root.find("enabled")->as_bool());
  CHECK(root.find("missing")->is_null());
  CHECK_NEAR(root.find("negative")->as_number(), -12.5, 1e-12);
  CHECK_NEAR(root.find("exponent")->as_number(), 1000.0, 1e-12);
  CHECK(root.find("absent") == nullptr);
}

TEST(json_round_trips_through_dump) {
  Json root = Json::object();
  root.set("cycles", Json::number(4830221));
  root.set("ipc", Json::number(2.07));
  root.set("label", Json::string("pointer chase \"fast\"\n"));
  Json list = Json::array();
  list.push_back(Json::number(1));
  list.push_back(Json::number(2));
  root.set("list", std::move(list));

  const Json reparsed = Json::parse(root.dump());
  CHECK_NEAR(reparsed.find("cycles")->as_number(), 4830221.0, 1e-12);
  CHECK_NEAR(reparsed.find("ipc")->as_number(), 2.07, 1e-12);
  CHECK_EQ(reparsed.find("label")->as_string(), std::string("pointer chase \"fast\"\n"));
  CHECK_EQ(reparsed.find("list")->elements().size(), 2u);
}

// Cycle and instruction counts are integers; printing them as 4830221.0 would
// make the result files awkward to read.
TEST(json_prints_integral_numbers_without_a_decimal_point) {
  Json root = Json::object();
  root.set("cycles", Json::number(4830221));
  const std::string text = root.dump(0);
  CHECK_EQ(text, std::string("{\"cycles\":4830221}"));
}

// JSON has no literal for infinity or NaN, so emitting one would produce a
// document that no conforming parser reads back, including the Python layer.
TEST(json_writes_non_finite_numbers_as_null) {
  Json root = Json::object();
  root.set("infinite", Json::number(std::numeric_limits<double>::infinity()));
  root.set("negative", Json::number(-std::numeric_limits<double>::infinity()));
  root.set("undefined", Json::number(std::numeric_limits<double>::quiet_NaN()));
  const std::string text = root.dump(0);
  CHECK_EQ(text, std::string("{\"infinite\":null,\"negative\":null,\"undefined\":null}"));
  // The point of writing null is that the document stays parseable.
  CHECK(Json::parse(text).find("infinite")->is_null());
}

TEST(json_keeps_object_keys_in_insertion_order) {
  Json root = Json::object();
  root.set("b", Json::number(1));
  root.set("a", Json::number(2));
  root.set("b", Json::number(3));  // updates in place
  CHECK_EQ(root.dump(0), std::string("{\"b\":3,\"a\":2}"));
}

TEST(json_rejects_malformed_input) {
  CHECK_THROWS(Json::parse("{\"a\": 1} trailing"));
  CHECK_THROWS(Json::parse("{\"a\": }"));
  CHECK_THROWS(Json::parse("[1, 2"));
  CHECK_THROWS(Json::parse("\"unterminated"));
  CHECK_THROWS(Json::parse("tru"));
  CHECK_THROWS(Json::parse_file("/nonexistent/perfsim/config.json"));
}

TEST(json_reports_the_wrong_type_instead_of_returning_garbage) {
  const Json root = Json::parse(R"({"a": "text"})");
  CHECK_THROWS(root.find("a")->as_number());
  CHECK_THROWS(root.elements());
}
