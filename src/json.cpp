#include "json.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace bwe {

const Json& Json::at(const std::string& key) const {
  if (type_ != Type::Object) throw std::runtime_error("JSON: not an object for key '" + key + "'");
  auto it = object_.find(key);
  if (it == object_.end()) throw std::runtime_error("JSON: missing key '" + key + "'");
  return it->second;
}

bool Json::has(const std::string& key) const {
  return type_ == Type::Object && object_.count(key) > 0;
}

double Json::as_number() const {
  if (type_ != Type::Number) throw std::runtime_error("JSON: value is not a number");
  return number_;
}
int Json::as_int() const { return static_cast<int>(as_number()); }
bool Json::as_bool() const {
  if (type_ != Type::Bool) throw std::runtime_error("JSON: value is not a bool");
  return bool_;
}
const std::string& Json::as_string() const {
  if (type_ != Type::String) throw std::runtime_error("JSON: value is not a string");
  return string_;
}

double Json::number_or(const std::string& key, double def) const {
  return has(key) ? at(key).as_number() : def;
}
int Json::int_or(const std::string& key, int def) const {
  return has(key) ? at(key).as_int() : def;
}
bool Json::bool_or(const std::string& key, bool def) const {
  return has(key) ? at(key).as_bool() : def;
}
std::string Json::string_or(const std::string& key, const std::string& def) const {
  return has(key) ? at(key).as_string() : def;
}

// --- Parser -----------------------------------------------------------------

class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : s_(text), i_(0) {}

  Json parse() {
    Json v = parse_value();
    skip_ws();
    if (i_ != s_.size()) fail("trailing characters after JSON document");
    return v;
  }

 private:
  const std::string& s_;
  size_t i_;

  [[noreturn]] void fail(const std::string& msg) {
    throw std::runtime_error("JSON parse error at offset " + std::to_string(i_) + ": " + msg);
  }

  void skip_ws() {
    while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) i_++;
  }

  char peek() {
    skip_ws();
    if (i_ >= s_.size()) fail("unexpected end of input");
    return s_[i_];
  }

  Json parse_value() {
    char c = peek();
    switch (c) {
      case '{': return parse_object();
      case '[': return parse_array();
      case '"': return parse_string_value();
      case 't':
      case 'f': return parse_bool();
      case 'n': return parse_null();
      default: return parse_number();
    }
  }

  Json parse_object() {
    Json v;
    v.type_ = Json::Type::Object;
    i_++;  // consume '{'
    skip_ws();
    if (peek() == '}') { i_++; return v; }
    while (true) {
      skip_ws();
      if (peek() != '"') fail("expected string key");
      std::string key = parse_string_raw();
      skip_ws();
      if (peek() != ':') fail("expected ':'");
      i_++;
      v.object_[key] = parse_value();
      skip_ws();
      char d = peek();
      if (d == ',') { i_++; continue; }
      if (d == '}') { i_++; break; }
      fail("expected ',' or '}'");
    }
    return v;
  }

  Json parse_array() {
    Json v;
    v.type_ = Json::Type::Array;
    i_++;  // consume '['
    skip_ws();
    if (peek() == ']') { i_++; return v; }
    while (true) {
      v.array_.push_back(parse_value());
      skip_ws();
      char d = peek();
      if (d == ',') { i_++; continue; }
      if (d == ']') { i_++; break; }
      fail("expected ',' or ']'");
    }
    return v;
  }

  std::string parse_string_raw() {
    if (s_[i_] != '"') fail("expected '\"'");
    i_++;
    std::string out;
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return out;
      if (c == '\\') {
        if (i_ >= s_.size()) fail("unterminated escape");
        char e = s_[i_++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          default: out += e; break;  // best-effort; ignore \uXXXX
        }
      } else {
        out += c;
      }
    }
    fail("unterminated string");
  }

  Json parse_string_value() {
    Json v;
    v.type_ = Json::Type::String;
    v.string_ = parse_string_raw();
    return v;
  }

  Json parse_bool() {
    Json v;
    v.type_ = Json::Type::Bool;
    if (s_.compare(i_, 4, "true") == 0) { v.bool_ = true; i_ += 4; }
    else if (s_.compare(i_, 5, "false") == 0) { v.bool_ = false; i_ += 5; }
    else fail("invalid literal");
    return v;
  }

  Json parse_null() {
    Json v;
    v.type_ = Json::Type::Null;
    if (s_.compare(i_, 4, "null") == 0) i_ += 4;
    else fail("invalid literal");
    return v;
  }

  Json parse_number() {
    size_t start = i_;
    if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) i_++;
    bool any = false;
    while (i_ < s_.size() &&
           (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' || s_[i_] == 'e' ||
            s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) {
      i_++;
      any = true;
    }
    if (!any) fail("invalid number");
    Json v;
    v.type_ = Json::Type::Number;
    v.number_ = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
    return v;
  }
};

Json Json::parse(const std::string& text) { return JsonParser(text).parse(); }

Json Json::parse_file(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open config file: " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return parse(ss.str());
}

}  // namespace bwe
