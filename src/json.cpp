#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace perfsim {
namespace {

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Json parse() {
    Json value = parse_value();
    skip_whitespace();
    if (pos_ != text_.size()) fail("trailing characters after JSON value");
    return value;
  }

 private:
  [[noreturn]] void fail(const std::string& message) const {
    std::ostringstream oss;
    oss << "JSON parse error at offset " << pos_ << ": " << message;
    throw JsonError(oss.str());
  }

  void skip_whitespace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
      ++pos_;
    }
  }

  char peek() {
    skip_whitespace();
    if (pos_ >= text_.size()) fail("unexpected end of input");
    return text_[pos_];
  }

  void expect(char c) {
    if (peek() != c) fail(std::string("expected '") + c + "'");
    ++pos_;
  }

  bool consume_literal(std::string_view literal) {
    if (text_.compare(pos_, literal.size(), literal) == 0) {
      pos_ += literal.size();
      return true;
    }
    return false;
  }

  Json parse_value() {
    switch (peek()) {
      case '{': return parse_object();
      case '[': return parse_array();
      case '"': return Json::string(parse_string());
      case 't':
        if (!consume_literal("true")) fail("invalid literal");
        return Json::boolean(true);
      case 'f':
        if (!consume_literal("false")) fail("invalid literal");
        return Json::boolean(false);
      case 'n':
        if (!consume_literal("null")) fail("invalid literal");
        return Json();
      default: return parse_number();
    }
  }

  Json parse_object() {
    expect('{');
    Json result = Json::object();
    if (peek() == '}') {
      ++pos_;
      return result;
    }
    while (true) {
      std::string key = parse_string();
      expect(':');
      result.set(std::move(key), parse_value());
      char c = peek();
      ++pos_;
      if (c == '}') return result;
      if (c != ',') fail("expected ',' or '}' in object");
    }
  }

  Json parse_array() {
    expect('[');
    Json result = Json::array();
    if (peek() == ']') {
      ++pos_;
      return result;
    }
    while (true) {
      result.push_back(parse_value());
      char c = peek();
      ++pos_;
      if (c == ']') return result;
      if (c != ',') fail("expected ',' or ']' in array");
    }
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    while (true) {
      if (pos_ >= text_.size()) fail("unterminated string");
      char c = text_[pos_++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) fail("unterminated escape sequence");
      char esc = text_[pos_++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: fail("unsupported escape sequence");
      }
    }
  }

  Json parse_number() {
    const char* begin = text_.data() + pos_;
    char* end = nullptr;
    double value = std::strtod(begin, &end);
    if (end == begin) fail("invalid number");
    pos_ += static_cast<size_t>(end - begin);
    return Json::number(value);
  }

  std::string_view text_;
  size_t pos_ = 0;
};

void append_escaped(std::string& out, const std::string& value) {
  out.push_back('"');
  for (char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c);
    }
  }
  out.push_back('"');
}

void append_number(std::string& out, double value) {
  // JSON has no literal for infinity or NaN, and printing one would produce a
  // document that no conforming parser will read back, including the Python
  // layer that consumes these results. Configuration validation rejects
  // non-finite inputs, so this is a backstop rather than an expected path.
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }
  char buffer[40];
  // Integral values are printed without a decimal point: cycle and instruction
  // counts read much better as 4830221 than as 4830221.0.
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.10g", value);
  }
  out += buffer;
}

}  // namespace

Json Json::boolean(bool v) {
  Json j;
  j.type_ = Type::Bool;
  j.bool_ = v;
  return j;
}

Json Json::number(double v) {
  Json j;
  j.type_ = Type::Number;
  j.number_ = v;
  return j;
}

Json Json::string(std::string v) {
  Json j;
  j.type_ = Type::String;
  j.string_ = std::move(v);
  return j;
}

Json Json::array() {
  Json j;
  j.type_ = Type::Array;
  return j;
}

Json Json::object() {
  Json j;
  j.type_ = Type::Object;
  return j;
}

Json Json::parse(std::string_view text) { return Parser(text).parse(); }

Json Json::parse_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw JsonError("cannot open JSON file: " + path);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string text = buffer.str();
  try {
    return parse(text);
  } catch (const JsonError& e) {
    throw JsonError(path + ": " + e.what());
  }
}

bool Json::as_bool() const {
  if (type_ != Type::Bool) throw JsonError("JSON value is not a boolean");
  return bool_;
}

double Json::as_number() const {
  if (type_ != Type::Number) throw JsonError("JSON value is not a number");
  return number_;
}

const std::string& Json::as_string() const {
  if (type_ != Type::String) throw JsonError("JSON value is not a string");
  return string_;
}

const std::vector<Json>& Json::elements() const {
  if (type_ != Type::Array) throw JsonError("JSON value is not an array");
  return elements_;
}

const std::vector<Json::Field>& Json::fields() const {
  if (type_ != Type::Object) throw JsonError("JSON value is not an object");
  return fields_;
}

const Json* Json::find(std::string_view key) const {
  if (type_ != Type::Object) return nullptr;
  for (const Field& field : fields_) {
    if (field.first == key) return &field.second;
  }
  return nullptr;
}

void Json::set(std::string key, Json value) {
  if (type_ != Type::Object) {
    *this = object();
  }
  for (Field& field : fields_) {
    if (field.first == key) {
      field.second = std::move(value);
      return;
    }
  }
  fields_.emplace_back(std::move(key), std::move(value));
}

void Json::push_back(Json value) {
  if (type_ != Type::Array) {
    *this = array();
  }
  elements_.push_back(std::move(value));
}

std::string Json::dump(int indent) const {
  std::string out;
  dump_to(out, indent, 0);
  if (indent > 0) out.push_back('\n');
  return out;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
  const bool pretty = indent > 0;
  const std::string pad = pretty ? std::string((depth + 1) * indent, ' ') : std::string();
  const std::string closing_pad = pretty ? std::string(depth * indent, ' ') : std::string();

  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: append_number(out, number_); break;
    case Type::String: append_escaped(out, string_); break;
    case Type::Array:
      if (elements_.empty()) {
        out += "[]";
        break;
      }
      out.push_back('[');
      for (size_t i = 0; i < elements_.size(); ++i) {
        if (i > 0) out.push_back(',');
        if (pretty) {
          out.push_back('\n');
          out += pad;
        }
        elements_[i].dump_to(out, indent, depth + 1);
      }
      if (pretty) {
        out.push_back('\n');
        out += closing_pad;
      }
      out.push_back(']');
      break;
    case Type::Object:
      if (fields_.empty()) {
        out += "{}";
        break;
      }
      out.push_back('{');
      for (size_t i = 0; i < fields_.size(); ++i) {
        if (i > 0) out.push_back(',');
        if (pretty) {
          out.push_back('\n');
          out += pad;
        }
        append_escaped(out, fields_[i].first);
        out.push_back(':');
        if (pretty) out.push_back(' ');
        fields_[i].second.dump_to(out, indent, depth + 1);
      }
      if (pretty) {
        out.push_back('\n');
        out += closing_pad;
      }
      out.push_back('}');
      break;
  }
}

}  // namespace perfsim
