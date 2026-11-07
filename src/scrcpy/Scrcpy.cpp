#include "scrcpy/Scrcpy.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <thread>

namespace {
const std::string scrcpy_name = std::string("scrcpy") + app::executable_suffix;
}

Scrcpy::Scrcpy(Output output, Session session): output_(std::move(output)), session_(std::move(session)) {}

Scrcpy::~Scrcpy() { stop(); }
bool Scrcpy::set_path(const std::string &value) {
  auto path = app::from_utf8(value);
#ifdef _WIN32
  auto extension = app::to_utf8(path.extension());
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  bool valid = app::is_file(path) && extension == ".exe";
#else
  std::error_code error;
  auto permissions = std::filesystem::status(path, error).permissions();
  bool valid = app::is_file(path) &&
               (permissions & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
#endif
  if (!valid) {
    output_("Select an existing " + scrcpy_name + " executable.\n");
    return false;
  }
  path_ = app::to_utf8(std::filesystem::absolute(path));
  output_("[scrcpy] " + path_ + "\n");
  return true;
}
bool Scrcpy::detect(const std::string &saved) {
  if (!saved.empty() && app::is_file(app::from_utf8(saved)))
    return set_path(saved);
  if (auto env = app::environment("SCRCPY"); !env.empty() && app::is_file(app::from_utf8(env)))
    return set_path(env);
  auto found = app::find_executable("scrcpy");
  if (!found.empty())
    return set_path(app::to_utf8(found));
  std::ifstream legacy(app::config_directory() / "scrcpy_path.txt",
                       std::ios::binary);
  std::string old((std::istreambuf_iterator<char>(legacy)),
                  std::istreambuf_iterator<char>());
  if (!old.empty() && app::is_file(app::from_utf8(old)))
    return set_path(old);
  output_("[scrcpy] Not found. Choose " + scrcpy_name + " in Settings.\n");
  return false;
}
std::string Scrcpy::adb_path() const {
  auto adjacent = app::from_utf8(path_).parent_path() / (std::string("adb") + app::executable_suffix);
  if (!path_.empty() && app::is_file(adjacent))
    return app::to_utf8(adjacent);
  auto found = app::find_executable("adb");
  // A bare name is resolved through PATH at launch time.
  return found.empty() ? std::string("adb") + app::executable_suffix : app::to_utf8(found);
}
void Scrcpy::version(std::function<void(std::string)> done) const {
  auto path = path_;
  std::thread([path, done] {
    auto result = app::run_process(path, {"--version"}, {}, 5000);
    auto line = app::trim(result.text.substr(0, result.text.find('\n')));
    done(result.code == 0 ? line : "scrcpy version unavailable");
  }).detach();
}
void Scrcpy::list_devices(Devices done) const {
  auto adb = adb_path();
  std::thread([adb, done] {
    auto result = app::run_process(adb, {"devices", "-l"});
    done(result.code == 0 ? app::parse_devices(result.text)
                          : std::vector<app::Device>{},
         result.code == 0 ? "" : app::trim(result.text));
  }).detach();
}
void Scrcpy::restart_adb(std::function<void()> done) const {
  auto adb = adb_path();
  auto output = output_;
  std::thread([adb, output, done] {
    output("[adb] Restarting server...\n");
    app::run_process(adb, {"kill-server"}, output);
    app::run_process(adb, {"start-server"}, output);
    done();
  }).detach();
}
bool Scrcpy::start(const std::string &serial, const std::string &arguments) {
  if (path_.empty() || serial.empty()) {
    output_("Choose " + scrcpy_name + " and a ready device first.\n");
    return false;
  }
  std::string error;
  auto args = app::template_args(arguments, error);
  if (!error.empty()) {
    output_(error + "\n");
    return false;
  }
  args.insert(args.begin(), "--serial=" + serial);
  auto cancel = std::make_shared<app::Cancel>();
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->sessions.count(serial)) {
      output_("This device already has a mirror session.\n");
      return false;
    }
    state_->sessions[serial] = cancel;
  }
  session_(serial, true);
  auto state = state_;
  auto path = path_;
  auto output = output_;
  auto session = session_;
  output("[mirror] Starting " + serial + " " + arguments + "\n");
  std::thread([state, path, args, output, session, serial, cancel] {
    auto result =
        app::run_process(path, args, output, 0, cancel,
                         app::from_utf8(path).parent_path());
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->sessions.erase(serial);
    }
    output("[mirror] " + serial + " stopped (exit " +
           std::to_string(result.code) + ").\n");
    session(serial, false);
  }).detach();
  return true;
}
void Scrcpy::stop(const std::string &serial) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  for (const auto &[id, cancel] : state_->sessions)
    if (serial.empty() || serial == id)
      cancel->requested = true;
}
