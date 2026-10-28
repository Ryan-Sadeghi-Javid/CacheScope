// cachescope -- a small, dependency-free JSON value type, parser and writer.
//
// Scope is deliberately narrow: enough to read configuration files and to emit
// counter snapshots / JSON Lines. It is not a general-purpose JSON library.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace cachescope {

class JsonError : public std::runtime_error {
 public:
  explicit JsonError(const std::string& what) : std::runtime_error(what) {}
};

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() : type_(Type::Null) {}
  Json(bool b) : type_(Type::Bool), bool_(b) {}
  Json(double d) : type_(Type::Number), num_(d) {}
  Json(int i) : type_(Type::Number), num_(static_cast<double>(i)) {}
  Json(long long i) : type_(Type::Number), num_(static_cast<double>(i)) {}
  Json(unsigned long long i) : type_(Type::Number), num_(static_cast<double>(i)) {}
  Json(const char* s) : type_(Type::String), str_(s) {}
  Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

  static Json array() { Json j; j.type_ = Type::Array; return j; }
  static Json object() { Json j; j.type_ = Type::Object; return j; }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool() const;
  double as_double() const;
  std::int64_t as_int() const;
  const std::string& as_string() const;

  const std::vector<Json>& items() const { return arr_; }
  std::vector<Json>& items() { return arr_; }
  const std::map<std::string, Json>& fields() const { return obj_; }

  // Object access. `has` is false for a missing key; a present-but-null key
  // also reports false so that `"llc": null` reads as "level absent".
  bool has(const std::string& key) const;
  const Json& at(const std::string& key) const;  // throws if missing
  const Json* find(const std::string& key) const;  // nullptr if missing

  void set(const std::string& key, Json value);
  void push_back(Json value);

  // Mutable access, used by the dotted-path setter behind --set. `ref` creates
  // a null member when the key is absent, turning the value into an object if
  // it was not one already.
  Json& ref(const std::string& key);
  Json* find_mut(const std::string& key);
  Json& item(std::size_t i);

  // Typed getters with defaults, for config reading.
  bool get_bool(const std::string& key, bool fallback) const;
  double get_double(const std::string& key, double fallback) const;
  std::int64_t get_int(const std::string& key, std::int64_t fallback) const;
  std::string get_string(const std::string& key, const std::string& fallback) const;

  static Json parse(const std::string& text);
  static Json parse_file(const std::string& path);

  std::string dump(int indent = -1) const;  // indent < 0 => compact

 private:
  void dump_to(std::string& out, int indent, int depth) const;

  Type type_;
  bool bool_ = false;
  double num_ = 0.0;
  std::string str_;
  std::vector<Json> arr_;
  std::map<std::string, Json> obj_;
};

}  // namespace cachescope
