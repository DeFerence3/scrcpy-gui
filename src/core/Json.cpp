#include "core/Json.h"
#include <cstdlib>

namespace app::json {
const Value *Value::get(const std::string &key) const {
  if (type != object) return nullptr;
  for (const auto &[name, value] : members)
    if (name == key) return &value;
  return nullptr;
}
std::string Value::str(const std::string &key, const std::string &fallback) const {
  auto value = get(key);
  return value && value->type == string ? value->text : fallback;
}
double Value::number_or(const std::string &key, double fallback) const {
  auto value = get(key);
  return value && value->type == number ? value->num : fallback;
}

namespace {
class Parser {
public:
  explicit Parser(const std::string &text) : s_(text) {}
  bool document(Value &out) {
    if (!value(out, 0)) return false;
    space();
    return i_ == s_.size();
  }

private:
  const std::string &s_;
  size_t i_ = 0;

  void space() {
    while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
  }
  bool literal(const char *word) {
    size_t n = 0;
    while (word[n]) ++n;
    if (s_.compare(i_, n, word) != 0) return false;
    i_ += n;
    return true;
  }
  static void append_utf8(std::string &out, unsigned long cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xc0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3f)); }
    else if (cp < 0x10000) {
      out += (char)(0xe0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3f)); out += (char)(0x80 | (cp & 0x3f));
    } else {
      out += (char)(0xf0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3f));
      out += (char)(0x80 | ((cp >> 6) & 0x3f)); out += (char)(0x80 | (cp & 0x3f));
    }
  }
  bool hex4(unsigned long &cp) {
    if (i_ + 4 > s_.size()) return false;
    cp = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s_[i_++];
      cp <<= 4;
      if (c >= '0' && c <= '9') cp |= (unsigned long)(c - '0');
      else if (c >= 'a' && c <= 'f') cp |= (unsigned long)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') cp |= (unsigned long)(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool string_value(std::string &out) {
    if (i_ >= s_.size() || s_[i_] != '"') return false;
    ++i_;
    while (i_ < s_.size()) {
      char c = s_[i_++];
      if (c == '"') return true;
      if ((unsigned char)c < 0x20) return false;
      if (c != '\\') { out += c; continue; }
      if (i_ >= s_.size()) return false;
      char e = s_[i_++];
      switch (e) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        unsigned long cp;
        if (!hex4(cp)) return false;
        if (cp >= 0xd800 && cp < 0xdc00) {
          unsigned long low;
          if (i_ + 2 > s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') return false;
          i_ += 2;
          if (!hex4(low) || low < 0xdc00 || low > 0xdfff) return false;
          cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
        } else if (cp >= 0xdc00 && cp <= 0xdfff)
          return false;
        append_utf8(out, cp);
        break;
      }
      default: return false;
      }
    }
    return false;
  }
  bool value(Value &out, int depth) {
    if (depth > 64) return false;
    space();
    if (i_ >= s_.size()) return false;
    char c = s_[i_];
    if (c == '{') {
      out.type = Value::object;
      ++i_;
      space();
      if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
      while (true) {
        space();
        std::string key;
        if (!string_value(key)) return false;
        space();
        if (i_ >= s_.size() || s_[i_++] != ':') return false;
        Value member;
        if (!value(member, depth + 1)) return false;
        out.members.emplace_back(std::move(key), std::move(member));
        space();
        if (i_ >= s_.size()) return false;
        if (s_[i_] == ',') { ++i_; continue; }
        if (s_[i_] == '}') { ++i_; return true; }
        return false;
      }
    }
    if (c == '[') {
      out.type = Value::array;
      ++i_;
      space();
      if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
      while (true) {
        Value item;
        if (!value(item, depth + 1)) return false;
        out.items.push_back(std::move(item));
        space();
        if (i_ >= s_.size()) return false;
        if (s_[i_] == ',') { ++i_; continue; }
        if (s_[i_] == ']') { ++i_; return true; }
        return false;
      }
    }
    if (c == '"') { out.type = Value::string; return string_value(out.text); }
    if (literal("true")) { out.type = Value::boolean; out.flag = true; return true; }
    if (literal("false")) { out.type = Value::boolean; return true; }
    if (literal("null")) { out.type = Value::null; return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
      size_t start = i_;
      if (s_[i_] == '-') ++i_;
      bool digits = false;
      while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' || s_[i_] == 'e' ||
                                s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) {
        digits |= s_[i_] >= '0' && s_[i_] <= '9';
        ++i_;
      }
      if (!digits) return false;
      auto token = s_.substr(start, i_ - start);
      char *end = nullptr;
      out.type = Value::number;
      out.num = std::strtod(token.c_str(), &end);
      return end && *end == '\0';
    }
    return false;
  }
};
}

bool parse(const std::string &text, Value &out) {
  out = Value{};
  return Parser(text).document(out);
}
}
