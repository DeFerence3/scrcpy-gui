#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace app {
inline constexpr char update_repository[] = "DeFerence3/scrcpy-gui-slint";

inline std::string repository_url() {
  return std::string("https://github.com/") + update_repository;
}

struct Device {
  std::string serial, name, state, connection;
  bool ready() const { return state == "device"; }
};
struct Template {
  std::string name, arguments;
};
struct Settings {
  std::string scrcpy_path;
  std::string shortcut = "Ctrl + Alt + S";
  bool auto_update = false;
  int default_template = 0;
  std::vector<Template> templates = {
      {"Default (1080p, 60fps)", "--max-size=1920 --max-fps=60"},
      {"Lightweight (720p, 30fps)","--max-size=1280 --max-fps=30 --video-bit-rate=4M --no-audio"},
      {"Full resolution no audio", "--no-audio"}};
};
std::vector<Device> parse_devices(const std::string &text);
std::string trim(std::string text);
bool load_settings(const std::filesystem::path &path, Settings &settings,std::string &error);
bool save_settings(const std::filesystem::path &path, const Settings &settings,std::string &error);
} // namespace app
