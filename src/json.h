#pragma once

// A tiny, dependency-free JSON parser — just enough to read config.json.
// Supports objects, arrays, strings, numbers, booleans and null. Not a
// general-purpose library (no unicode escapes beyond the common ones), but
// sufficient for the fixed-shape config in this experiment.
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bwe {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() : type_(Type::Null) {}

  Type type() const { return type_; }
  bool is_object() const { return type_ == Type::Object; }
  bool is_array() const { return type_ == Type::Array; }

  // Object access. Throws std::runtime_error if key missing or wrong type.
  const Json& at(const std::string& key) const;
  bool has(const std::string& key) const;

  // Array access.
  const std::vector<Json>& items() const { return array_; }

  // Scalars. as_number/as_int/as_bool/as_string throw on type mismatch.
  double as_number() const;
  int as_int() const;
  bool as_bool() const;
  const std::string& as_string() const;

  // Convenience: read key with a default if absent.
  double number_or(const std::string& key, double def) const;
  int int_or(const std::string& key, int def) const;
  bool bool_or(const std::string& key, bool def) const;
  std::string string_or(const std::string& key, const std::string& def) const;

  // Parse a full JSON document. Throws std::runtime_error on malformed input.
  static Json parse(const std::string& text);
  static Json parse_file(const std::string& path);

 private:
  Type type_;
  bool bool_ = false;
  double number_ = 0;
  std::string string_;
  std::vector<Json> array_;
  std::map<std::string, Json> object_;

  friend class JsonParser;
};

}  // namespace bwe
