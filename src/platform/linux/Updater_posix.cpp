#include "platform/linux/Updater.h"
#include "core/Json.h"
#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace update {
namespace {
constexpr char user_agent[] = "scrcpy-gui-updater";
constexpr uint64_t max_archive = 300ull << 20;

std::vector<unsigned long> version_parts(const std::string &version) {
  if (!valid_version(version)) throw std::invalid_argument("Invalid version: " + version);
  std::vector<unsigned long> parts;
  std::istringstream in(version);
  std::string part;
  while (std::getline(in, part, '.')) parts.push_back(std::stoul(part));
  parts.resize(4, 0);
  return parts;
}
std::string first_word(const std::string &text) { return text.substr(0, text.find_first_of(" \t\r\n")); }
std::string trim(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
  return text;
}
std::string random_name() {
  std::random_device device;
  std::ostringstream name;
  name << std::hex << device() << device();
  return name.str();
}

// Downloads `url` into `file` with curl; returns the HTTP status, 0 on failure.
int fetch(const std::string &url, const std::filesystem::path &file, unsigned timeout_s, std::string &error,
          bool api = false) {
  std::vector<std::string> args{"--proto", "=https", "--tlsv1.2", "-sS", "-L", "--max-time", std::to_string(timeout_s),
                                "-A", user_agent, "-o", app::to_utf8(file), "-w", "%{http_code}"};
  if (api) {
    args.insert(args.end(), {"-H", "Accept: application/vnd.github+json", "-H", "X-GitHub-Api-Version: 2022-11-28"});
  }
  args.push_back(url);
  auto result = app::run_process("curl", args, {}, (timeout_s + 10) * 1000);
  if (result.code != 0) {
    error = trim(result.text).empty() ? "curl failed (is curl installed?)" : trim(result.text);
    return 0;
  }
  auto out = trim(result.text); // -w prints the status code last
  try {
    return std::stoi(out.substr(out.size() >= 3 ? out.size() - 3 : 0));
  } catch (...) {
    error = "Unexpected curl output.";
    return 0;
  }
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string check_archive(const std::filesystem::path &archive) {
  auto names = app::run_process("tar", {"-tzf", app::to_utf8(archive)}, {}, 60000);
  auto types = app::run_process("tar", {"-tvzf", app::to_utf8(archive)}, {}, 60000);
  if (names.code != 0 || types.code != 0) return "The release archive could not be read.";
  std::istringstream type_lines(types.text);
  std::string line;
  while (std::getline(type_lines, line))
    if (!line.empty() && line[0] != '-' && line[0] != 'd') return "The release archive contains links or special files.";
  std::istringstream name_lines(names.text);
  bool executable = false;
  while (std::getline(name_lines, line)) {
    if (line.empty() || line.back() == '/') continue;
    if (line.rfind("./", 0) == 0) line.erase(0, 2);
    if (!allowed_entry(line)) return "Unexpected release file: " + line;
    executable |= line == "bin/scrcpy-ui";
  }
  return executable ? "" : "The release is missing bin/scrcpy-ui.";
}

bool has_asset(const std::string &json, const std::string &name) {
  app::json::Value release;
  if (!app::json::parse(json, release)) return true; // parse_release reports it
  auto assets = release.get("assets");
  if (!assets || assets->type != app::json::Value::array) return false;
  for (const auto &item : assets->items)
    if (item.str("name") == name) return true;
  return false;
}

app::ProcessResult finish(app::ProcessResult result, const std::string &marker, const std::string &value) {
  result.code = 0;
  result.text += marker + "|" + value + "\n";
  return result;
}
app::ProcessResult fail(app::ProcessResult result, const std::string &message) {
  result.code = 1;
  result.text += "[update] " + message + "\n";
  return result;
}
}

std::string asset_name() {
#if defined(__aarch64__)
  return "scrcpy-gui-linux-arm64.tar.gz";
#else
  return "scrcpy-gui-linux-x64.tar.gz";
#endif
}

bool valid_version(const std::string &version) {
  int parts = 0;
  size_t digits = 0;
  for (char c : version) {
    if (c >= '0' && c <= '9') {
      if (++digits > 9) return false;
    } else if (c == '.' && digits) {
      ++parts;
      digits = 0;
    } else
      return false;
  }
  return digits && (parts == 2 || parts == 3);
}

int compare_versions(const std::string &a, const std::string &b) {
  auto x = version_parts(a), y = version_parts(b);
  return x < y ? -1 : x > y ? 1 : 0;
}

std::string parse_release(const std::string &json, const std::string &repository, const std::string &asset,
                          std::string &version, Asset &result) {
  app::json::Value release;
  if (!app::json::parse(json, release) || release.type != app::json::Value::object)
    return "GitHub returned an unreadable release.";
  version = release.str("tag_name");
  if (!version.empty() && version[0] == 'v') version.erase(0, 1);
  if (!valid_version(version)) return "Release tags must use vMAJOR.MINOR.PATCH.";
  auto assets = release.get("assets");
  if (!assets || assets->type != app::json::Value::array) return "The release lists no assets.";
  for (const auto &item : assets->items) {
    if (item.str("name") != asset) continue;
    result.url = item.str("browser_download_url");
    auto prefix = "https://github.com/" + repository + "/releases/download/";
    if (result.url.compare(0, prefix.size(), prefix) != 0)
      return "Release download URL does not belong to the configured repository.";
    auto digest = item.str("digest");
    // Fail closed when GitHub has not supplied a digest for the asset.
    if (digest.size() != 71 || digest.compare(0, 7, "sha256:") != 0 ||
        digest.find_first_not_of("0123456789abcdefABCDEF", 7) != std::string::npos)
      return "This release has no GitHub SHA-256 digest. Re-upload the release asset.";
    result.sha256 = digest.substr(7);
    std::transform(result.sha256.begin(), result.sha256.end(), result.sha256.begin(), ::tolower);
    double size = item.number_or("size", 0);
    if (size <= 0 || size > (double)max_archive) return "Release archive has an unexpected size.";
    result.size = (uint64_t)size;
    return {};
  }
  return "Release " + version + " has no " + asset + " asset.";
}

bool allowed_entry(const std::string &name) {
  if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos || name.find('\\') != std::string::npos)
    return false;
  if (name == "bin/scrcpy-ui" || name == "share/applications/scrcpy-gui.desktop" ||
      name == "share/icons/hicolor/scalable/apps/scrcpy-gui.svg")
    return true;
  for (const char *lib : {"lib/libslint_cpp.so", "lib64/libslint_cpp.so"})
    if (name.rfind(lib, 0) == 0 && name.find('/', std::string(lib).size()) == std::string::npos) return true;
  const std::string doc = "share/doc/scrcpy-gui/";
  return name.rfind(doc, 0) == 0 && name.size() > doc.size() && name.find('/', doc.size()) == std::string::npos;
}

std::filesystem::path state_directory() {
  auto cache = app::environment("XDG_CACHE_HOME");
  auto base = cache.empty() ? app::from_utf8(app::environment("HOME")) / ".cache" : app::from_utf8(cache);
  return base / "scrcpy-tray" / "updates";
}

std::filesystem::path install_prefix(std::string &reason) {
  auto bin = app::executable_directory();
  if (bin.filename() != "bin") {
    reason = "This copy is not an installed release";
    return {};
  }
  auto prefix = bin.parent_path();
  if (prefix == "/usr") {
    reason = "This copy is managed by your package manager";
    return {};
  }
  if (access(bin.c_str(), W_OK) != 0 || access(prefix.c_str(), W_OK) != 0) {
    reason = "This copy is installed in " + app::to_utf8(prefix) + ", which you cannot write to";
    return {};
  }
  return prefix;
}

app::ProcessResult run(bool stage, const std::string &repository, const std::string &current,
                       const std::string &expected) {
  app::ProcessResult result;
  try {
    auto root = state_directory();
    std::filesystem::create_directories(root);
    // Previous stages are never reused.
    for (const auto &entry : std::filesystem::directory_iterator(root))
      if (entry.is_directory() && entry.path().filename().string().rfind("stage-", 0) == 0)
        std::filesystem::remove_all(entry.path());
    auto work = root / ("stage-" + random_name());
    std::filesystem::create_directories(work);
    auto metadata = work / "release.json";
    std::string error;
    int status = fetch("https://api.github.com/repos/" + repository + "/releases/latest", metadata, 30, error, true);
    if (status == 404) {
      std::filesystem::remove_all(work);
      return finish(result, "__NONE__", "No published release found (or repository is private).");
    }
    if (status != 200) return fail(result, error.empty() ? "GitHub returned HTTP " + std::to_string(status) + "." : error);
    std::string version;
    Asset asset;
    auto json = read_file(metadata);
    // Version comparison only needs the tag, so a missing asset is still reported.
    app::json::Value release;
    if (app::json::parse(json, release)) {
      auto tag = release.str("tag_name");
      if (!tag.empty() && tag[0] == 'v') tag.erase(0, 1);
      if (valid_version(tag) && compare_versions(tag, current) <= 0) {
        std::filesystem::remove_all(work);
        return finish(result, "__NONE__", "You are up to date.");
      }
    }
    if (!has_asset(json, asset_name())) {
      std::filesystem::remove_all(work);
      return finish(result, "__NONE__", "The latest release has no Linux package yet.");
    }
    error = parse_release(json, repository, asset_name(), version, asset);
    if (!error.empty()) return fail(result, error);
    std::string reason;
    if (!stage) {
      std::filesystem::remove_all(work);
      if (install_prefix(reason).empty()) {
        result.text += "[update] " + reason + "; update it the way you installed it.\n";
        return finish(result, "__MANUAL__", version);
      }
      return finish(result, "__AVAILABLE__", version);
    }
    if (install_prefix(reason).empty()) return fail(result, reason + ".");
    if (!expected.empty() && expected != version) return fail(result, "The latest release changed. Check for updates again.");
    auto archive = work / "release.tar.gz";
    result.text += "[update] Downloading GUI " + version + "...\n";
    // Like update.ps1, prefer the download proxy; the digest check guards both.
    status = fetch("https://gh-proxy.com/" + asset.url, archive, 180, error);
    std::error_code size_error;
    if (status != 200 || std::filesystem::file_size(archive, size_error) != asset.size) {
      result.text += "[update] Proxy download failed; trying GitHub directly.\n";
      status = fetch(asset.url, archive, 180, error);
    }
    if (status != 200) return fail(result, error.empty() ? "Download failed with HTTP " + std::to_string(status) + "." : error);
    auto hash = app::run_process("sha256sum", {app::to_utf8(archive)}, {}, 60000);
    if (hash.code != 0 || first_word(hash.text) != asset.sha256) return fail(result, "Release SHA-256 verification failed.");
    error = extract(archive, work / "files");
    if (!error.empty()) return fail(result, error);
    return finish(result, "__READY__", app::to_utf8(work));
  } catch (const std::exception &e) {
    return fail(result, e.what());
  }
}

std::string extract(const std::filesystem::path &archive, const std::filesystem::path &directory) {
  auto error = check_archive(archive);
  if (!error.empty()) return error;
  std::filesystem::create_directories(directory);
  auto result = app::run_process("tar", {"-xzf", app::to_utf8(archive), "-C", app::to_utf8(directory), "--no-same-owner"},
                                 {}, 120000);
  return result.code == 0 ? "" : "The release archive could not be extracted.";
}

std::string apply(const std::filesystem::path &stage, const std::filesystem::path &prefix) {
  namespace fs = std::filesystem;
  struct Step {
    fs::path target, backup;
    bool existed;
  };
  std::vector<Step> done;
  std::error_code ec;
  try {
    auto files = stage / "files";
    if (!fs::is_regular_file(files / "bin" / "scrcpy-ui")) return "The staged update is incomplete.";
    std::vector<fs::path> entries;
    for (auto it = fs::recursive_directory_iterator(files); it != fs::recursive_directory_iterator(); ++it) {
      if (it->is_symlink() || (!it->is_regular_file() && !it->is_directory())) return "The staged update is invalid.";
      if (!it->is_regular_file()) continue;
      auto relative = it->path().lexically_relative(files).generic_string();
      if (!allowed_entry(relative)) return "Unexpected staged file: " + relative;
      entries.push_back(it->path().lexically_relative(files));
    }
    // Libraries first and the executable last, so a failure leaves the old app runnable.
    std::stable_sort(entries.begin(), entries.end(), [](const fs::path &a, const fs::path &b) {
      return (a.generic_string() == "bin/scrcpy-ui") < (b.generic_string() == "bin/scrcpy-ui");
    });
    for (const auto &relative : entries) {
      auto target = prefix / relative;
      fs::create_directories(target.parent_path());
      auto incoming = target;
      incoming += ".scrcpy-new";
      fs::copy_file(files / relative, incoming, fs::copy_options::overwrite_existing);
      fs::permissions(incoming, fs::status(files / relative).permissions());
      Step step{target, target, fs::exists(target)};
      step.backup += ".scrcpy-old";
      if (step.existed) fs::rename(target, step.backup);
      done.push_back(step);
      fs::rename(incoming, target);
    }
  } catch (const std::exception &e) {
    for (auto it = done.rbegin(); it != done.rend(); ++it) {
      auto incoming = it->target;
      incoming += ".scrcpy-new";
      fs::remove(incoming, ec);
      if (it->existed) fs::rename(it->backup, it->target, ec);
      else fs::remove(it->target, ec);
    }
    return std::string("Installing the update failed: ") + e.what();
  }
  for (const auto &step : done)
    if (step.existed) fs::remove(step.backup, ec);
  fs::remove_all(stage, ec);
  return {};
}
}
