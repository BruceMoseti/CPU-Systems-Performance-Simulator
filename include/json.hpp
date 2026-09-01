// Minimal JSON reader/writer.
//
// perfsim needs exactly two things from JSON: read small configuration files
// and write small result files. A full-featured library would be far more code
// than the rest of the simulator, so this implements the subset that is used:
// objects, arrays, numbers, strings, booleans and null.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace perfsim {

class JsonError : public std::runtime_error {
 public:
  explicit JsonError(const std::string& what) : std::runtime_error(what) {}
};

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  // Object fields keep insertion order so that emitted files are stable and
  // diffable, which matters when results are committed or compared.
  using Field = std::pair<std::string, Json>;

  Json() = default;
  static Json boolean(bool v);
  static Json number(double v);
  static Json string(std::string v);
  static Json array();
  static Json object();

  static Json parse(std::string_view text);
  static Json parse_file(const std::string& path);

  std::string dump(int indent = 2) const;

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_object() const { return type_ == Type::Object; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_number() const { return type_ == Type::Number; }

  bool as_bool() const;
  double as_number() const;
  const std::string& as_string() const;
  const std::vector<Json>& elements() const;
  const std::vector<Field>& fields() const;

  // Returns nullptr when the key is absent, so callers can apply defaults.
  const Json* find(std::string_view key) const;

  void set(std::string key, Json value);
  void push_back(Json value);

 private:
  void dump_to(std::string& out, int indent, int depth) const;

  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<Json> elements_;
  std::vector<Field> fields_;
};

}  // namespace perfsim
