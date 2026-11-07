#include "platform/Platform.h"
#include <chrono>

// Platform-independent helpers. OS-specific parts live in Platform_win.cpp and
// Platform_posix.cpp.
namespace app {
// Works whether the build uses C++17 (char) or C++20 (char8_t) u8 strings.
std::string to_utf8(const std::filesystem::path &path) {
  auto value = path.u8string();
  return std::string(value.begin(), value.end());
}
std::filesystem::path from_utf8(const std::string &value) {
#ifdef __cpp_char8_t
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
#else
  return std::filesystem::u8path(value);
#endif
}

uint64_t milliseconds() {
  using namespace std::chrono;
  return (uint64_t)duration_cast<std::chrono::milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::vector<std::string> split_args(const std::string &text) {
  // CommandLineToArgvW rules: 2n backslashes + quote -> n backslashes and a
  // quote toggle; 2n+1 backslashes + quote -> n backslashes and a literal
  // quote; other backslashes are literal; "" inside quotes is a literal quote.
  std::vector<std::string> result;
  std::string current;
  bool quoted = false, any = false;
  size_t i = 0;
  while (i < text.size()) {
    char c = text[i];
    if (c == '\\') {
      size_t slashes = 0;
      while (i < text.size() && text[i] == '\\') { ++slashes; ++i; }
      if (i < text.size() && text[i] == '"') {
        current.append(slashes / 2, '\\');
        if (slashes % 2) { current += '"'; ++i; }
      } else
        current.append(slashes, '\\');
      any = true;
      continue;
    }
    if (c == '"') {
      if (quoted && i + 1 < text.size() && text[i + 1] == '"') { current += '"'; i += 2; continue; }
      quoted = !quoted; any = true; ++i;
      continue;
    }
    if (!quoted && (c == ' ' || c == '\t')) {
      if (any) { result.push_back(std::move(current)); current.clear(); any = false; }
      ++i;
      continue;
    }
    current += c; any = true; ++i;
  }
  if (any) result.push_back(std::move(current));
  return result;
}

std::vector<std::string> template_args(const std::string &text, std::string &error) {
  if (text.size() > 4096 || text.find_first_of("\r\n") != std::string::npos) {
    error = "Arguments must be a single line of at most 4096 characters."; return {};
  }
  // Reject unmatched quotes; the argv rules would otherwise silently accept them.
  bool quoted = false;
  size_t slashes = 0;
  for (char c : text) {
    if (c == '"' && slashes % 2 == 0) quoted = !quoted;
    slashes = c == '\\' ? slashes + 1 : 0;
  }
  if (quoted) { error = "Close the quoted argument before saving."; return {}; }
  std::vector<std::string> result;
  for (auto &arg : split_args(text)) {
    auto name = arg.substr(0, arg.find('='));
    if (name == "--serial" || name == "--select-usb" || name == "--select-tcpip" || name == "--tcpip"
        || name == "-d" || name == "-e" || arg.rfind("-s", 0) == 0) {
      error = "Choose devices in the Devices tab; remove device-selection arguments.";
      return {};
    }
    result.push_back(std::move(arg));
  }
  return result;
}
}
