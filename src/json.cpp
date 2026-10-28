#include "cachescope/json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace cachescope {
namespace {

// Recursive-descent parser.
class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}

  Json parse() {
    skip_ws();
    Json v = parse_value();
    skip_ws();
    if (pos_ != s_.size()) throw JsonError(err("trailing characters after JSON value"));
    return v;
  }

 private:
  const std::string& s_;
  std::size_t pos_ = 0;

  std::string err(const std::string& msg) const {
    // Report line/column so a typo in a config file is findable.
    std::size_t line = 1, col = 1;
    for (std::size_t i = 0; i < pos_ && i < s_.size(); ++i) {
      if (s_[i] == '\n') { ++line; col = 1; } else { ++col; }
    }
    std::ostringstream os;
    os << "JSON parse error at line " << line << ", column " << col << ": " << msg;
    return os.str();
  }

  bool eof() const { return pos_ >= s_.size(); }
  char peek() const { return eof() ? '\0' : s_[pos_]; }

  void skip_ws() {
    while (!eof()) {
      char c = s_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
        // Line comments are not standard JSON but are invaluable in config
        // files, so we accept them.
        while (!eof() && s_[pos_] != '\n') ++pos_;
      } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < s_.size() && !(s_[pos_] == '*' && s_[pos_ + 1] == '/')) ++pos_;
        if (pos_ + 1 >= s_.size()) throw JsonError(err("unterminated block comment"));
        pos_ += 2;
      } else {
        break;
      }
    }
  }

  void expect(char c) {
    if (peek() != c) throw JsonError(err(std::string("expected ") + c));
    ++pos_;
  }

  bool literal(const char* lit) {
    std::size_t n = std::strlen(lit);
    if (s_.compare(pos_, n, lit) == 0) { pos_ += n; return true; }
    return false;
  }

  Json parse_value() {
    if (eof()) throw JsonError(err("unexpected end of input"));
    switch (peek()) {
      case '{': return parse_object();
      case '[': return parse_array();
      case '"': return Json(parse_string());
      case 't': if (literal("true")) return Json(true); throw JsonError(err("bad literal"));
      case 'f': if (literal("false")) return Json(false); throw JsonError(err("bad literal"));
      case 'n': if (literal("null")) return Json(); throw JsonError(err("bad literal"));
      default: return parse_number();
    }
  }

  Json parse_object() {
    expect('{');
    Json obj = Json::object();
    skip_ws();
    if (peek() == '}') { ++pos_; return obj; }
    for (;;) {
      skip_ws();
      if (peek() != '"') throw JsonError(err("expected object key string"));
      std::string key = parse_string();
      skip_ws();
      expect(':');
      skip_ws();
      obj.set(key, parse_value());
      skip_ws();
      if (peek() == ',') { ++pos_; continue; }
      if (peek() == '}') { ++pos_; return obj; }
      throw JsonError(err("expected comma or closing brace in object"));
    }
  }

  Json parse_array() {
    expect('[');
    Json arr = Json::array();
    skip_ws();
    if (peek() == ']') { ++pos_; return arr; }
    for (;;) {
      skip_ws();
      arr.push_back(parse_value());
      skip_ws();
      if (peek() == ',') { ++pos_; continue; }
      if (peek() == ']') { ++pos_; return arr; }
      throw JsonError(err("expected comma or closing bracket in array"));
    }
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    for (;;) {
      if (eof()) throw JsonError(err("unterminated string"));
      char c = s_[pos_++];
      if (c == '"') return out;
      if (c != '\\') { out.push_back(c); continue; }
      if (eof()) throw JsonError(err("unterminated escape"));
      char e = s_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (pos_ + 4 > s_.size()) throw JsonError(err("short unicode escape"));
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            char h = s_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9') code |= unsigned(h - '0');
            else if (h >= 'a' && h <= 'f') code |= unsigned(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= unsigned(h - 'A' + 10);
            else throw JsonError(err("bad hex digit in unicode escape"));
          }
          // Encode as UTF-8. Surrogate pairs are not reassembled; config files
          // in this project are ASCII.
          if (code < 0x80) {
            out.push_back(static_cast<char>(code));
          } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
          break;
        }
        default: throw JsonError(err("unknown escape character"));
      }
    }
  }

  Json parse_number() {
    std::size_t start = pos_;
    if (peek() == '-' || peek() == '+') ++pos_;
    bool any = false;
    while (!eof() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) { ++pos_; any = true; }
    if (!eof() && s_[pos_] == '.') {
      ++pos_;
      while (!eof() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) { ++pos_; any = true; }
    }
    if (!any) throw JsonError(err("invalid number"));
    if (!eof() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      ++pos_;
      if (!eof() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
      while (!eof() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    return Json(std::stod(s_.substr(start, pos_ - start)));
  }
};

void append_escaped(std::string& out, const std::string& s) {
  out.push_back('"');
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c & 0xFF);
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

// Numbers round-trip as integers when exactly integral, so counter dumps read
// as "l1d.misses": 1234 rather than 1234.0.
void append_number(std::string& out, double v) {
  char buf[40];
  if (!std::isfinite(v)) {
    // JSON has no NaN/Infinity; emit null so downstream parsers stay happy.
    out += "null";
    return;
  }
  if (v == std::floor(v) && std::fabs(v) < 9.0e15) {
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  } else {
    std::snprintf(buf, sizeof(buf), "%.10g", v);
  }
  out += buf;
}

}  // namespace

bool Json::as_bool() const {
  if (type_ == Type::Bool) return bool_;
  if (type_ == Type::Number) return num_ != 0.0;
  throw JsonError("JSON value is not a bool");
}

double Json::as_double() const {
  if (type_ == Type::Number) return num_;
  throw JsonError("JSON value is not a number");
}

std::int64_t Json::as_int() const {
  if (type_ != Type::Number) throw JsonError("JSON value is not a number");
  return static_cast<std::int64_t>(num_ < 0 ? num_ - 0.5 : num_ + 0.5);
}

const std::string& Json::as_string() const {
  if (type_ != Type::String) throw JsonError("JSON value is not a string");
  return str_;
}

bool Json::has(const std::string& key) const {
  const Json* v = find(key);
  return v != nullptr && !v->is_null();
}

const Json* Json::find(const std::string& key) const {
  if (type_ != Type::Object) return nullptr;
  auto it = obj_.find(key);
  return it == obj_.end() ? nullptr : &it->second;
}

const Json& Json::at(const std::string& key) const {
  const Json* v = find(key);
  if (v == nullptr) throw JsonError("missing required JSON key: " + key);
  return *v;
}

void Json::set(const std::string& key, Json value) {
  type_ = Type::Object;
  obj_[key] = std::move(value);
}

void Json::push_back(Json value) {
  type_ = Type::Array;
  arr_.push_back(std::move(value));
}

Json& Json::ref(const std::string& key) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    arr_.clear();
  }
  return obj_[key];
}

Json* Json::find_mut(const std::string& key) {
  if (type_ != Type::Object) return nullptr;
  auto it = obj_.find(key);
  return it == obj_.end() ? nullptr : &it->second;
}

Json& Json::item(std::size_t i) {
  if (type_ != Type::Array || i >= arr_.size()) {
    throw JsonError("JSON array index out of range: " + std::to_string(i));
  }
  return arr_[i];
}

bool Json::get_bool(const std::string& key, bool fallback) const {
  const Json* v = find(key);
  return (v && !v->is_null()) ? v->as_bool() : fallback;
}

double Json::get_double(const std::string& key, double fallback) const {
  const Json* v = find(key);
  return (v && !v->is_null()) ? v->as_double() : fallback;
}

std::int64_t Json::get_int(const std::string& key, std::int64_t fallback) const {
  const Json* v = find(key);
  return (v && !v->is_null()) ? v->as_int() : fallback;
}

std::string Json::get_string(const std::string& key, const std::string& fallback) const {
  const Json* v = find(key);
  return (v && !v->is_null()) ? v->as_string() : fallback;
}

Json Json::parse(const std::string& text) { return Parser(text).parse(); }

Json Json::parse_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw JsonError("cannot open JSON file: " + path);
  std::ostringstream os;
  os << in.rdbuf();
  return parse(os.str());
}

std::string Json::dump(int indent) const {
  std::string out;
  dump_to(out, indent, 0);
  return out;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
  const bool pretty = indent >= 0;
  auto newline = [&](int d) {
    if (!pretty) return;
    out.push_back('\n');
    out.append(static_cast<std::size_t>(indent * d), ' ');
  };

  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: append_number(out, num_); break;
    case Type::String: append_escaped(out, str_); break;
    case Type::Array: {
      if (arr_.empty()) { out += "[]"; break; }
      out.push_back('[');
      for (std::size_t i = 0; i < arr_.size(); ++i) {
        if (i) out.push_back(',');
        newline(depth + 1);
        arr_[i].dump_to(out, indent, depth + 1);
      }
      newline(depth);
      out.push_back(']');
      break;
    }
    case Type::Object: {
      if (obj_.empty()) { out += "{}"; break; }
      out.push_back('{');
      bool first = true;
      for (const auto& kv : obj_) {
        if (!first) out.push_back(',');
        first = false;
        newline(depth + 1);
        append_escaped(out, kv.first);
        out.push_back(':');
        if (pretty) out.push_back(' ');
        kv.second.dump_to(out, indent, depth + 1);
      }
      newline(depth);
      out.push_back('}');
      break;
    }
  }
}

}  // namespace cachescope
