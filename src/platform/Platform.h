#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Portable process, path and OS helpers. All strings are UTF-8; Windows-only
// translation units convert at the Win32 boundary.
namespace app {
#ifdef _WIN32
inline constexpr char executable_suffix[] = ".exe";
std::wstring wide(const std::string &value);
std::string utf8(const std::wstring &value);
// Quotes one argument for CreateProcess / CommandLineToArgvW.
std::wstring quote_arg(const std::wstring &value);
bool parse_hotkey(const std::string &text, UINT &modifiers, UINT &key, std::string &canonical);
#else
inline constexpr char executable_suffix[] = "";
#endif

std::string to_utf8(const std::filesystem::path &path);
std::filesystem::path from_utf8(const std::string &value);
// Splits a command line with the Windows argv rules on every platform, so
// templates saved on one OS parse identically on another.
std::vector<std::string> split_args(const std::string &text);
std::vector<std::string> template_args(const std::string &text, std::string &error);

std::filesystem::path config_directory();
std::filesystem::path executable_directory();
std::string environment(const char *name);
unsigned long process_id();
std::string last_error();
uint64_t milliseconds();

bool is_file(const std::filesystem::path &path);
// Searches PATH; `name` has no extension (".exe" is added on Windows).
std::filesystem::path find_executable(const std::string &name);
// Creates `path` exclusively. Returns an error message, empty on success.
std::string create_new_file(const std::filesystem::path &path, bool &exists);
std::string replace_file(const std::filesystem::path &from, const std::filesystem::path &to);
bool open_url(const std::string &url, std::string &error);

struct Cancel {
  std::atomic<bool> requested{false};
};
struct ProcessResult {
  int code = 1;
  std::string text;
  bool timed_out = false;
};
using Output = std::function<void(std::string)>;
// Runs an executable directly (never through a shell), capturing stdout and
// stderr. Cancelling or timing out terminates the child and its descendants,
// except daemons that detach themselves (such as the shared ADB server).
ProcessResult run_process(const std::string &executable,
                          const std::vector<std::string> &args,
                          Output output = {}, unsigned timeout_ms = 30000,
                          std::shared_ptr<Cancel> cancel = {},
                          const std::filesystem::path &directory = {});
bool launch_detached(const std::string &executable,
                     const std::vector<std::string> &args, std::string &error);
}
