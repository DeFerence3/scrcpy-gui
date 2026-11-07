#include "core/AppCore.h"
#include "platform/Platform.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace app {
std::string trim(std::string text) {
  auto first = text.find_first_not_of(" \r\n\t");
  if (first == std::string::npos)
    return {};
  return text.substr(first, text.find_last_not_of(" \r\n\t") - first + 1);
}
std::vector<Device> parse_devices(const std::string &text) {
  std::vector<Device> devices;
  std::istringstream input(text);
  std::string line;
  bool listing = false;
  while (std::getline(input, line)) {
    line = trim(line);
    if (line.rfind("List of devices attached", 0) == 0) {
      listing = true;
      continue;
    }
    if (!listing || line.empty() || line[0] == '*')
      continue;
    std::istringstream fields(line);
    Device d;
    fields >> d.serial >> d.state;
    if (d.state != "device" && d.state != "offline" &&
        d.state != "unauthorized" && d.state != "recovery" &&
        d.state != "sideload" && d.state != "bootloader" && d.state != "no")
      continue;
    if (d.state == "no")
      d.state = "no permissions";
    d.name = d.serial;
    d.connection = d.serial.find(':') != std::string::npos ||
                           d.serial.find("_tcp") != std::string::npos
                       ? "Wireless"
                       : "USB";
    if (d.serial.rfind("emulator-", 0) == 0)
      d.connection = "Emulator";
    std::string field;
    while (fields >> field) {
      if (field.rfind("model:", 0) == 0) {
        d.name = field.substr(6);
        std::replace(d.name.begin(), d.name.end(), '_', ' ');
      }
    }
    if (std::none_of(devices.begin(), devices.end(), [&](const Device &other) {
          return d.serial == other.serial;
        }))
      devices.push_back(std::move(d));
  }
  return devices;
}
bool load_settings(const std::filesystem::path &path, Settings &settings,
                   std::string &error) {
  if (!std::filesystem::exists(path))
    return true;
  std::ifstream in(path, std::ios::binary);
  Settings next;
  std::string magic;
  int version = 0, count = 0;
  in >> magic >> version;
  in >> std::quoted(next.scrcpy_path) >> std::quoted(next.shortcut);
  if (version == 1) {
    // Older settings included a user-selected update repository. Ignore it.
    std::string legacy_repository;
    in >> std::quoted(legacy_repository);
  }
  in >> next.auto_update >> next.default_template >> count;
  if (!in || magic != "SCRCPY_PAD" || (version != 1 && version != 2) || count < 1 ||
      count > 100) {
    error = "Settings file is invalid; using defaults without overwriting it.";
    return false;
  }
  next.templates.clear();
  for (int i = 0; i < count; ++i) {
    Template t;
    in >> std::quoted(t.name) >> std::quoted(t.arguments);
    if (!in || t.name.empty() || t.name.size() > 80 ||
        t.arguments.size() > 4096) {
      error = "Could not read saved templates.";
      return false;
    }
    next.templates.push_back(std::move(t));
  }
  if (next.default_template < 0 || next.default_template >= count)
    next.default_template = 0;
  settings = std::move(next);
  return true;
}
bool save_settings(const std::filesystem::path &path, const Settings &settings,
                   std::string &error) {
  try {
    std::filesystem::create_directories(path.parent_path());
    auto temp = path;
    temp += ".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << "SCRCPY_PAD 2\n"
        << std::quoted(settings.scrcpy_path) << '\n'
        << std::quoted(settings.shortcut) << '\n'
        << settings.auto_update << ' ' << settings.default_template << ' '
        << settings.templates.size() << '\n';
    for (const auto &t : settings.templates)
      out << std::quoted(t.name) << ' ' << std::quoted(t.arguments) << '\n';
    out.flush();
    if (!out) {
      error = "Unable to write settings.";
      return false;
    }
    out.close();
    if (auto failure = replace_file(temp, path); !failure.empty()) {
      error = "Unable to replace settings: " + failure;
      return false;
    }
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
}
} // namespace app
