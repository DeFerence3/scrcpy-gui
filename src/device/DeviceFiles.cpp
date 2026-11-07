#include "device/DeviceFiles.h"
#include "core/AppCore.h"
#include <algorithm>
#include <charconv>
#include <stdexcept>

namespace app {
namespace {
std::string shell_quote(const std::string &value) {
  std::string result = "'";
  for (char ch : value)
    result += ch == '\'' ? "'\\''" : std::string(1, ch);
  return result + "'";
}
std::string command_error(const ProcessResult &result, const std::shared_ptr<Cancel> &cancel) {
  if (cancel && cancel->requested)
    return "Operation cancelled.";
  if (result.code == 0)
    return {};
  auto error = trim(result.text);
  return error.empty() ? "ADB command failed (exit " + std::to_string(result.code) + ")." : error;
}
}
std::string device_path(const std::string &path) {
  if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos)
    throw std::invalid_argument("Enter an absolute device path, such as /sdcard/Download.");
  std::vector<std::string> parts;
  size_t start = 1;
  while (start < path.size()) {
    auto end = path.find('/', start);
    auto part = path.substr(start, end == std::string::npos ? end : end - start);
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!part.empty() && part != ".") parts.push_back(part);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  std::string result;
  for (const auto &part : parts) result += "/" + part;
  return result.empty() ? "/" : result;
}
std::string device_child(const std::string &folder, const std::string &name) {
  if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
      name.find('\0') != std::string::npos)
    throw std::invalid_argument("Invalid device filename.");
  auto path = device_path(folder);
  return path + (path == "/" ? "" : "/") + name;
}
std::string device_parent(const std::string &path) {
  auto normalized = device_path(path);
  auto slash = normalized.rfind('/');
  return slash == 0 ? "/" : normalized.substr(0, slash);
}
DirectoryResult list_device_files(const std::string &adb, const std::string &serial,
                                 const std::string &path, std::shared_ptr<Cancel> cancel) {
  DirectoryResult listing;
  try {
    // NUL-delimited fields preserve spaces, quotes, Unicode and newlines in names.
    // The end marker also detects truncated process output instead of showing a
    // misleading partial directory. Never interpolate unquoted remote paths.
    auto script = "cd " + shell_quote(device_path(path)) + " && { "
      "ls -a > /dev/null || exit; printf 'SCRCPY_FILES\\0'; "
      "for f in .[!.]* ..?* *; do "
      "[ -e \"$f\" ] || [ -L \"$f\" ] || continue; "
      "if [ -d \"$f\" ]; then t=d; s=0; "
      "elif [ -f \"$f\" ]; then t=f; s=$(stat -L -c %s -- \"$f\") || exit; "
      "else continue; fi; printf '%s\\0%s\\0%s\\0' \"$t\" \"$s\" \"$f\"; "
      "done; printf 'SCRCPY_END\\0'; }";
    auto result = run_process(adb, {"-s", serial, "shell", "-T", script}, {}, 30000, cancel);
    listing.error = command_error(result, cancel);
    if (!listing.error.empty() && result.text.find('\0') != std::string::npos)
      listing.error = "Could not finish reading this folder. Refresh or choose another path.";
    if (!listing.error.empty()) return listing;
    size_t pos = 0;
    auto field = [&]() {
      auto end = result.text.find('\0', pos);
      if (end == std::string::npos) throw std::runtime_error("Incomplete directory listing. Try a smaller folder or refresh.");
      auto value = result.text.substr(pos, end - pos);
      pos = end + 1;
      return value;
    };
    // ADB can emit daemon startup messages before the actual shell output.
    auto begin = result.text.find(std::string("SCRCPY_FILES\0", 13));
    if (begin == std::string::npos) throw std::runtime_error("ADB returned an invalid directory listing.");
    pos = begin + 13;
    for (;;) {
      auto type = field();
      if (type == "SCRCPY_END") break;
      auto size = field();
      auto name = field();
      uint64_t bytes = 0;
      auto parsed = std::from_chars(size.data(), size.data() + size.size(), bytes);
      if ((type != "d" && type != "f") || parsed.ec != std::errc() || parsed.ptr != size.data() + size.size())
        throw std::runtime_error("ADB returned invalid file information.");
      device_child(path, name);
      listing.files.push_back({name, type == "d", bytes});
    }
    std::sort(listing.files.begin(), listing.files.end(), [](const auto &a, const auto &b) {
      return a.directory != b.directory ? a.directory : a.name < b.name;
    });
  } catch (const std::exception &e) { listing.error = e.what(); }
  if (!listing.error.empty()) listing.files.clear();
  return listing;
}
std::string download_device_file(const std::string &adb, const std::string &serial,
                                const std::string &remote, const std::filesystem::path &local,
                                std::shared_ptr<Cancel> cancel) {
  std::filesystem::path temporary;
  try {
    auto path = device_path(remote);
    static std::atomic<unsigned> next{0};
    // Reserve our own temporary path; never reuse someone else's partial file.
    for (;;) {
      temporary = local.parent_path() / (".scrcpy-" + std::to_string(process_id()) +
          "-" + std::to_string(++next) + ".part");
      bool exists = false;
      auto failure = create_new_file(temporary, exists);
      if (failure.empty()) break;
      if (!exists) { temporary.clear(); return failure; }
    }
    auto result = run_process(adb, {"-s", serial, "pull", path, to_utf8(temporary)}, {}, 0, cancel);
    auto error = command_error(result, cancel);
    if (cancel && cancel->requested) error = "Download cancelled.";
    if (error.empty()) error = replace_file(temporary, local);
    std::error_code ignored;
    if (!error.empty()) std::filesystem::remove(temporary, ignored);
    return error;
  } catch (const std::exception &e) {
    std::error_code ignored;
    if (!temporary.empty()) std::filesystem::remove(temporary, ignored);
    return e.what();
  }
}
std::string upload_device_files(const std::string &adb, const std::string &serial,
                               const std::string &folder, const std::vector<std::filesystem::path> &files,
                               std::shared_ptr<Cancel> cancel, Output progress) {
  try {
    auto destination = device_path(folder);
    if (destination.back() != '/') destination += '/';
    if (files.empty()) return "Choose at least one file to upload.";
    for (const auto &file : files)
      if (!std::filesystem::is_regular_file(file)) return "Only local files can be uploaded: " + to_utf8(file);
    size_t done = 0;
    for (const auto &file : files) {
      if (cancel && cancel->requested) return "Upload cancelled. Some files may already have been copied.";
      if (progress) progress("Uploading " + std::to_string(++done) + "/" + std::to_string(files.size()) + ": " + to_utf8(file.filename()));
      auto result = run_process(adb, {"-s", serial, "push", to_utf8(std::filesystem::absolute(file)), destination}, {}, 0, cancel);
      auto error = command_error(result, cancel);
      if (cancel && cancel->requested) error = "Upload cancelled. A partial file may remain on the device.";
      if (!error.empty()) return "Upload stopped at " + to_utf8(file.filename()) + ": " + error;
    }
    return {};
  } catch (const std::exception &e) { return e.what(); }
}
}
