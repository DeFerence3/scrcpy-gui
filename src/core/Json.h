#pragma once
#include <string>
#include <utility>
#include <vector>

// A small JSON reader for the few documents the app consumes (GitHub release
// metadata, Hyprland IPC replies). Numbers are kept as doubles.
namespace app::json {
struct Value {
  enum Type { null, boolean, number, string, array, object } type = null;
  bool flag = false;
  double num = 0;
  std::string text;
  std::vector<Value> items;                          // array elements
  std::vector<std::pair<std::string, Value>> members; // object members, in order

  // Member lookup; returns nullptr when absent or when this is not an object.
  const Value *get(const std::string &key) const;
  // Convenience accessors with a fallback for missing or mistyped members.
  std::string str(const std::string &key, const std::string &fallback = {}) const;
  double number_or(const std::string &key, double fallback = 0) const;
};
// Returns false (leaving `out` unspecified) for malformed or deeply nested input.
bool parse(const std::string &text, Value &out);
}
