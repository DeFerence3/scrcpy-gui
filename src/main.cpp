#include "main.h"
#include "platform/Desktop.h"
#include "device/DeviceModel.h"
#include "device/FileExplorer.h"
#include "scrcpy/Scrcpy.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <ole2.h>
#else
#include "platform/linux/Updater.h"
#endif


#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif

namespace {
std::string text(const slint::SharedString &s) {
  return std::string(s.data(), s.size());
}
const std::string scrcpy_name = std::string("scrcpy") + app::executable_suffix;
constexpr bool updates_supported = true;
#ifdef _WIN32
// Self-update is implemented by update.ps1 on Windows and Updater_posix.cpp on Linux.
std::string powershell() {
  wchar_t system[32768]{};
  GetSystemDirectoryW(system, 32768);
  return app::utf8(system) + "\\WindowsPowerShell\\v1.0\\powershell.exe";
}
#endif

class Controller : public std::enable_shared_from_this<Controller> {
public:
  slint::ComponentHandle<MainWindow> ui = MainWindow::create();
  app::Settings settings;
  std::unique_ptr<Scrcpy> scrcpy;
  std::shared_ptr<FileExplorer> explorer;
  std::unique_ptr<desktop::Integration> desktop;
  bool visible = false, picking = false, focused = false, pending_quick = false;
  bool shutting_down = false, settings_writable = true, installing = false,
       auto_install_attempted = false;
  uint64_t shown_at = 0, hidden_at = 0;
  int hotkey_id = 0;
  std::vector<app::Device> devices;
  std::shared_ptr<DeviceModel> device_model = std::make_shared<DeviceModel>();
  std::set<std::string> sessions;
  std::string available_version;
  std::filesystem::path settings_path;
  slint::Timer focus_timer, device_timer, update_timer;

  static void post(std::weak_ptr<Controller> weak,
                   std::function<void(Controller &)> callback) {
    slint::invoke_from_event_loop([weak, callback = std::move(callback)] {
      if (auto self = weak.lock(); self && !self->shutting_down)
        callback(*self);
    });
  }
  void log(std::string message) {
    auto value = text(ui->get_console_text()) + std::move(message);
    constexpr size_t limit = 48000;
    if (value.size() > limit) {
      auto end = value.find('\n', value.size() - limit);
      if (end != std::string::npos)
        value.erase(0, end + 1);
      else {
        // Never start the log in the middle of a UTF-8 character.
        auto start = value.size() - limit;
        while (start < value.size() && (static_cast<unsigned char>(value[start]) & 0xc0) == 0x80)
          ++start;
        value.erase(0, start);
      }
    }
    ui->set_console_text(slint::SharedString(value));
  }
  void status(const std::string &message) {
    ui->set_status_text(slint::SharedString(message));
  }
  bool save() {
    if (!settings_writable) {
      status("Settings could not be loaded. Back up or repair settings.dat "
             "before saving.");
      return false;
    }
    std::string error;
    if (!app::save_settings(settings_path, settings, error)) {
      log(error + "\n");
      status(error);
      return false;
    }
    return true;
  }
  void show(bool toggle = false) {
    if (toggle && app::milliseconds() - hidden_at < 400)
      return;
    if (visible && toggle) {
      hide();
      return;
    }
    ui->window().set_minimized(false);
    ui->show();
    // On Wayland a window created while hidden maps only once it draws a
    // frame; Slint does not schedule one for the first show after --startup.
    ui->window().request_redraw();
    visible = true;
    focused = false;
    shown_at = app::milliseconds();
    desktop::present_window();
    if (explorer)
      explorer->attach(desktop::native_window());
  }
  void hide() {
    if (explorer)
      explorer->detach();
    ui->hide();
    ui->set_page(0);
    visible = focused = false;
    hidden_at = app::milliseconds();
  }
  void quit() {
    shutting_down = true;
    focus_timer.stop();
    device_timer.stop();
    update_timer.stop();
    scrcpy->stop();
    if (explorer)
      explorer->shutdown();
    // The tray itself is destroyed after the event loop returns, because this
    // may run inside a tray callback.
    if (hotkey_id && desktop)
      desktop->unregister_hotkey(hotkey_id);
    hotkey_id = 0;
    slint::quit_event_loop();
  }
  void publish_devices() {
    std::vector<DeviceRow> rows;
    for (const auto &d : devices) {
      bool running = sessions.count(d.serial) != 0;
      std::string detail = d.connection + " · " + d.serial;
      if (!d.ready())
        detail += " · " + d.state;
      if (running)
        detail += " · mirroring";
      DeviceRow row;
      row.serial = slint::SharedString(d.serial);
      row.name = slint::SharedString(d.name);
      row.detail = slint::SharedString(detail);
      row.ready = d.ready();
      row.running = running;
      rows.push_back(row);
    }
    for (const auto &serial : sessions) {
      if (std::none_of(devices.begin(), devices.end(),
                       [&](const auto &d) { return d.serial == serial; })) {
        DeviceRow row;
        row.serial = slint::SharedString(serial);
        row.name = slint::SharedString(serial);
        row.detail = "Mirror session · device disconnected";
        row.ready = false;
        row.running = true;
        rows.push_back(row);
      }
    }
    device_model->update(rows);
    ui->set_active_sessions((int)sessions.size());
  }
  void publish_templates() {
    std::vector<slint::SharedString> names;
    for (const auto &t : settings.templates)
      names.emplace_back(t.name);
    ui->set_template_names(
        std::make_shared<slint::VectorModel<slint::SharedString>>(names));
    ui->set_default_template(settings.default_template);
  }
  void edit_template(int index) {
    if (index < 0 || index >= (int)settings.templates.size())
      return;
    ui->set_editing_template(index);
    ui->set_template_name(slint::SharedString(settings.templates[index].name));
    ui->set_template_arguments(
        slint::SharedString(settings.templates[index].arguments));
  }
  void save_template() {
    auto name = app::trim(text(ui->get_template_name()));
    auto arguments = app::trim(text(ui->get_template_arguments()));
    if (name.empty() || name.size() > 80) {
      status("Enter a template name (1–80 characters).");
      return;
    }
    std::string error;
    app::template_args(arguments, error);
    if (!error.empty()) {
      status(error);
      return;
    }
    int index = ui->get_editing_template();
    for (int i = 0; i < (int)settings.templates.size(); ++i) {
      if (i != index && settings.templates[i].name == name) {
        status("A template with this name already exists.");
        return;
      }
    }
    auto previous = settings;
    if (index < 0) {
      if (settings.templates.size() >= 100) {
        status("You can save up to 100 templates.");
        return;
      }
      index = (int)settings.templates.size();
      settings.templates.push_back({name, arguments});
    } else if (index < (int)settings.templates.size())
      settings.templates[index] = {name, arguments};
    else
      return;
    if (!save()) {
      settings = previous;
      return;
    }
    publish_templates();
    edit_template(index);
    status("Template saved. Use Make Default here to use it for Quick Mirror.");
  }
  void delete_template() {
    int index = ui->get_editing_template();
    if (index < 0 || index >= (int)settings.templates.size() ||
        settings.templates.size() <= 1)
      return;
    auto previous = settings;
    settings.templates.erase(settings.templates.begin() + index);
    if (settings.default_template == index)
      settings.default_template = 0;
    else if (settings.default_template > index)
      --settings.default_template;
    if (!save()) {
      settings = previous;
      return;
    }
    publish_templates();
    edit_template(settings.default_template);
    status("Template deleted.");
  }
  void mirror(const std::string &serial, int template_index) {
    if (installing) {
      status("Wait for the update installation to finish.");
      return;
    }
    if (scrcpy->path().empty()) {
      ui->set_page(1);
      show();
      status("Choose " + scrcpy_name + " before mirroring.");
      return;
    }
    auto d =
        std::find_if(devices.begin(), devices.end(),
                     [&](const auto &item) { return item.serial == serial; });
    if (d == devices.end() || !d->ready()) {
      status("That device is not ready. Refresh and accept USB debugging on "
             "the phone.");
      return;
    }
    if (template_index < 0 || template_index >= (int)settings.templates.size())
      return;
    if (scrcpy->start(serial, settings.templates[template_index].arguments))
      status("Starting " + d->name + " with " +
             settings.templates[template_index].name + ".");
  }
  void explore(const std::string &serial) {
    if (installing) {
      status("Wait for the update installation to finish.");
      return;
    }
    auto device =
        std::find_if(devices.begin(), devices.end(),
                     [&](const auto &d) { return d.serial == serial; });
    if (device == devices.end() || !device->ready()) {
      status("That device is not ready. Refresh and accept USB debugging on "
             "the phone.");
      return;
    }
    if (!explorer)
      explorer = FileExplorer::create(ui);
    explorer->open(desktop::native_window(), scrcpy->adb_path(),
                   serial, device->name);
  }
  bool show_if_mirroring() {
    if (sessions.empty())
      return false;
    pending_quick = false;
    ui->set_page(0);
    show();
    status("Mirroring is already active. Manage your devices.");
    return true;
  }
  void refresh(bool quick = false, bool silent = false) {
    if (quick && show_if_mirroring())
      return;
    pending_quick |= quick;
    if (ui->get_scanning()) {
      if (!silent)
        ui->set_refreshing(true);
      return;
    }
    if (quick && scrcpy->path().empty()) {
      pending_quick = false;
      ui->set_page(1);
      show();
      status("Select " + scrcpy_name + " in Settings to get started.");
      return;
    }
    ui->set_scanning(true);
    ui->set_refreshing(!silent);
    auto weak = weak_from_this();
    scrcpy->list_devices([weak, silent](std::vector<app::Device> list,
                                        std::string error) {
      post(weak, [list = std::move(list), error, silent](Controller &c) {
        c.ui->set_scanning(false);
        c.ui->set_refreshing(false);
        bool quick = c.pending_quick;
        c.pending_quick = false;
        // A session may have started while this device scan was in flight.
        if (quick && c.show_if_mirroring())
          return;
        if (!error.empty()) {
          if (!silent || quick) {
            c.log("[adb] " + error + "\n");
            c.status("ADB unavailable. Check the scrcpy folder or PATH; "
                     "details are in Logs.");
          }
          if (quick)
            c.show();
          return;
        }
        c.devices = list;
        c.publish_devices();
        if (!silent || quick)
          c.log("[adb] Found " + std::to_string(list.size()) + " device(s).\n");
        if (quick) {
          std::vector<app::Device> ready;
          for (const auto &d : list)
            if (d.ready())
              ready.push_back(d);
          if (ready.size() == 1)
            c.mirror(ready.front().serial, c.settings.default_template);
          else {
            c.ui->set_page(0);
            c.show();
            c.status(ready.empty() ? "No ready device. Connect a phone and "
                                     "accept USB debugging."
                                   : "Choose a device below to mirror with the "
                                     "default template.");
          }
        } else if (!silent)
          c.status(list.empty()
                       ? "Connect a phone and accept the USB debugging prompt."
                       : "Choose a device to mirror.");
      });
    });
  }
  void path_changed() {
    settings.scrcpy_path = scrcpy->path();
    ui->set_scrcpy_path(slint::SharedString(settings.scrcpy_path));
    save();
    auto weak = weak_from_this();
    auto expected = settings.scrcpy_path;
    scrcpy->version([weak, expected](std::string version) {
      post(weak, [version, expected](Controller &c) {
        if (c.settings.scrcpy_path == expected)
          c.ui->set_scrcpy_version(slint::SharedString(version));
      });
    });
    refresh();
  }
  void browse() {
    if (picking || installing)
      return;
    picking = true;
    desktop::FileDialog dialog;
    dialog.title = "Select " + scrcpy_name;
    dialog.filters = {{"scrcpy executable", scrcpy_name},
                      {"All executables", std::string("*") + app::executable_suffix}};
    auto weak = weak_from_this();
    desktop::choose_files(dialog, [weak](std::vector<std::filesystem::path> paths, std::string error) {
      post(weak, [paths = std::move(paths), error](Controller &c) {
        c.picking = false;
        if (!error.empty())
          c.status(error);
        else if (!paths.empty() && c.scrcpy->set_path(app::to_utf8(paths.front()))) {
          c.path_changed();
          c.status("scrcpy path saved.");
        }
        c.show();
      });
    });
  }
  bool apply_shortcut(const std::string &value, bool persist) {
    std::string canonical, error;
    if (!desktop->hotkeys_supported())
      return false;
    int new_id = desktop->register_hotkey(value, canonical, error);
    if (new_id < 0) {
      status("Use modifiers plus A–Z, 0–9 or F1–F24 (for example Ctrl + Alt + "
             "S).");
      return false;
    }
    if (hotkey_id && canonical == settings.shortcut) {
      if (new_id > 0)
        desktop->unregister_hotkey(new_id);
      ui->set_shortcut_edit(slint::SharedString(canonical));
      status("Shortcut is already active.");
      return true;
    }
    if (new_id == 0) {
      status("Shortcut unavailable; another app may be using it. Choose a "
             "different combination.");
      log("[shortcut] " + error + "\n");
      return false;
    }
    auto previous = settings.shortcut;
    settings.shortcut = canonical;
    if (persist && !save()) {
      settings.shortcut = previous;
      desktop->unregister_hotkey(new_id);
      return false;
    }
    if (hotkey_id)
      desktop->unregister_hotkey(hotkey_id);
    hotkey_id = new_id;
    ui->set_shortcut(slint::SharedString(canonical));
    ui->set_shortcut_edit(slint::SharedString(canonical));
    if (persist)
      status("Quick Mirror shortcut saved.");
    return true;
  }
  void save_update_settings() {
    if (ui->get_updating()) {
      status("Wait for the update operation to finish before changing its "
             "settings.");
      return;
    }
    auto previous = settings.auto_update;
    settings.auto_update = ui->get_auto_update();
    if (!save()) {
      settings.auto_update = previous;
      ui->set_auto_update(settings.auto_update);
      return;
    }
    status("Update settings saved.");
    if (settings.auto_update)
      update(false);
  }
  void update(bool install) {
    if (!updates_supported || ui->get_updating())
      return;
    if (install && (!sessions.empty() || picking || ui->get_scanning() ||
                    pending_quick || (explorer && explorer->busy()))) {
      status("Stop mirroring and wait for device discovery and file transfers "
             "before updating.");
      return;
    }
    if (install && available_version.empty())
      return;
    auto script = app::executable_directory() / "update.ps1";
#ifdef _WIN32
    if (!std::filesystem::exists(script)) {
      ui->set_update_status(
          "update.ps1 is missing from the application folder.");
      return;
    }
#endif
    ui->set_updating(true);
    installing = install;
    if (explorer)
      explorer->set_installing(install);
    auto_install_attempted = install;
    ui->set_update_status(install ? "Downloading and verifying the GUI update…"
                                  : "Checking GitHub releases…");
    auto expected = available_version;
    auto weak = weak_from_this();
    std::thread([weak, script, expected, install] {
#ifdef _WIN32
      std::vector<std::string> args{"-NoProfile",
                                    "-NonInteractive",
                                    "-ExecutionPolicy",
                                    "Bypass",
                                    "-File",
                                    app::to_utf8(script),
                                    "-Mode",
                                    install ? "Stage" : "Check",
                                    "-Repository",
                                    app::update_repository,
                                    "-CurrentVersion",
                                    APP_VERSION,
                                    "-ExpectedVersion",
                                    expected,
                                    "-Architecture",
#if defined(_M_ARM64)
                                    "arm64"
#elif defined(_WIN64)
                                    "x64"
#else
                                    "x86"
#endif
      };
      auto result =
          app::run_process(powershell(), args, {}, install ? 240000 : 45000);
#else
      (void)script; // update.ps1 is Windows-only.
      auto result = update::run(install, app::update_repository, APP_VERSION, expected);
#endif
      post(weak, [result, install](Controller &c) {
        c.ui->set_updating(false);
        c.installing = false;
        if (c.explorer)
          c.explorer->set_installing(false);
        std::istringstream lines(result.text);
        std::string line, marker, value;
        while (std::getline(lines, line)) {
          line = app::trim(line);
          if (line.rfind("__", 0) == 0) {
            auto split = line.find('|');
            marker = line.substr(0, split);
            value = split == std::string::npos ? "" : line.substr(split + 1);
          } else if (!line.empty())
            c.log(line + "\n");
        }
        if (result.code != 0) {
          c.ui->set_update_status("Update failed. See Logs for details.");
          return;
        }
        if (marker == "__NONE__") {
          c.available_version.clear();
          c.ui->set_update_available(false);
          c.ui->set_update_status(slint::SharedString(value));
        } else if (marker == "__MANUAL__") {
          // Installed by a package manager or into a read-only prefix.
          c.available_version.clear();
          c.ui->set_update_available(false);
          c.ui->set_update_status(slint::SharedString(
              "GUI " + value + " is available. Update it the way you installed it, "
              "or download it from the repository's Releases page."));
        } else if (marker == "__AVAILABLE__") {
          c.available_version = value;
          c.ui->set_update_available(true);
          c.ui->set_update_status(
              slint::SharedString("GUI " + value + " is available."));
          if (c.settings.auto_update && c.sessions.empty() && !c.picking &&
              !c.ui->get_scanning() && !c.pending_quick)
            c.update(true);
          else if (c.settings.auto_update)
            c.ui->set_update_status("Update available; installation waits "
                                    "until mirroring has stopped.");
        } else if (marker == "__READY__" && install)
          c.apply_update(app::from_utf8(value));
        else
          c.ui->set_update_status(
              "Unexpected update response. See Logs for details.");
      });
    }).detach();
  }
  void apply_update(const std::filesystem::path &stage) {
#ifdef _WIN32
    try {
      auto helper = stage / "apply-helper.ps1";
      std::filesystem::copy_file(
          app::executable_directory() / "update.ps1", helper,
          std::filesystem::copy_options::overwrite_existing);
      std::string error;
      if (!app::launch_detached(
              powershell(),
              {"-NoProfile", "-NonInteractive", "-ExecutionPolicy",
               "Bypass", "-File", app::to_utf8(helper), "-Mode", "Apply",
               "-StageDirectory", app::to_utf8(stage), "-InstallDirectory",
               app::to_utf8(app::executable_directory()), "-ParentProcessId",
               std::to_string(app::process_id())},
              error)) {
        log(error + "\n");
        ui->set_update_status("Could not start the update helper.");
        return;
      }
      quit();
    } catch (const std::exception &e) {
      log(std::string(e.what()) + "\n");
      ui->set_update_status("Unable to stage update helper.");
    }
#else
    std::string reason;
    auto prefix = update::install_prefix(reason);
    auto error = prefix.empty() ? reason : update::apply(stage, prefix);
    if (!error.empty()) {
      log("[update] " + error + "\n");
      ui->set_update_status("Update failed. See Logs for details.");
      return;
    }
    std::ofstream(update::state_directory() / "result.txt", std::ios::trunc)
        << "GUI update installed successfully.\n";
    // Hand the instance lock to the new copy before starting it.
    desktop::release_single_instance();
    std::error_code ignored;
    auto exe = app::to_utf8(std::filesystem::read_symlink("/proc/self/exe", ignored));
    if (!app::launch_detached(exe, {"--updated"}, error))
      log("[update] Installed, but could not restart: " + error + "\n");
    quit();
#endif
  }
  void initialize(bool updated, bool startup) {
    settings_path = app::config_directory() / "settings.dat";
    std::string error;
    settings_writable = app::load_settings(settings_path, settings, error);
    if (!error.empty())
      log(error + "\n");
    ui->set_app_version(APP_VERSION);
    ui->set_repository_url(slint::SharedString(app::repository_url()));
    ui->set_devices(device_model);
    ui->set_auto_update(settings.auto_update);
    publish_templates();
    edit_template(0);
    auto weak = weak_from_this();
    scrcpy = std::make_unique<Scrcpy>(
        [weak](std::string output) {
          post(weak,
               [output = std::move(output)](Controller &c) { c.log(output); });
        },
        [weak](std::string serial, bool running) {
          post(weak, [serial, running](Controller &c) {
            if (running)
              c.sessions.insert(serial);
            else
              c.sessions.erase(serial);
            c.publish_devices();
            if (!running && c.sessions.empty() && c.settings.auto_update &&
                !c.auto_install_attempted && c.ui->get_update_available())
              c.update(true);
          });
        });
    desktop::Events events;
    events.activate = [this] {
      show(true);
      if (visible)
        refresh(false, true);
    };
    events.toggle = [this] { show(true); };
    events.quick_mirror = [this] { refresh(true); };
    events.second_instance = [this] {
      show();
      refresh(true);
    };
    events.quit = [this] { quit(); };
    desktop = desktop::Integration::create(std::move(events));
    ui->set_shortcut_supported(desktop->hotkeys_supported());
    ui->set_updates_supported(updates_supported);
#ifndef _WIN32
    ui->set_scrcpy_placeholder("/usr/bin/scrcpy");
    ui->set_rounded_frame(false);
#endif
    ui->set_shortcut_edit(slint::SharedString(settings.shortcut));
    if (desktop->hotkeys_supported() && !apply_shortcut(settings.shortcut, false))
      ui->set_shortcut("Shortcut unavailable");
    ui->on_refresh_devices([this] { refresh(); });
    ui->on_mirror_device(
        [this](slint::SharedString serial, int template_index) {
          mirror(text(serial), template_index);
        });
    ui->on_explore_device(
        [this](slint::SharedString serial) { explore(text(serial)); });
    ui->on_stop_device(
        [this](slint::SharedString serial) { scrcpy->stop(text(serial)); });
    ui->on_stop_all([this] { scrcpy->stop(); });
    ui->on_browse_scrcpy([this] { browse(); });
    ui->on_detect_scrcpy([this] {
      if (scrcpy->detect())
        path_changed();
      else
        status("scrcpy was not found on PATH. Use Browse to select it.");
    });
    ui->on_save_path([this] {
      if (scrcpy->set_path(app::trim(text(ui->get_scrcpy_path())))) {
        path_changed();
        status("scrcpy path saved.");
      } else
        status("Invalid executable path. Select an existing " + scrcpy_name + ".");
    });
    ui->on_restart_adb([this, weak] {
      if (explorer && explorer->busy()) {
        status("Wait for the file operation to finish before restarting ADB.");
        return;
      }
      if (ui->get_scanning())
        return;
      ui->set_scanning(true);
      scrcpy->restart_adb([weak] {
        post(weak, [](Controller &c) {
          c.ui->set_scanning(false);
          c.refresh();
        });
      });
    });
    ui->on_save_shortcut(
        [this] { apply_shortcut(text(ui->get_shortcut_edit()), true); });
    ui->on_save_update_settings([this] { save_update_settings(); });
    ui->on_check_updates([this] { update(false); });
    ui->on_install_update([this] { update(true); });
    ui->on_open_repository([this] {
      std::string error;
      if (!app::open_url(text(ui->get_repository_url()), error))
        status("Could not open the GitHub repository in your browser.");
    });
    ui->on_make_default([this] {
      int index = ui->get_editing_template();
      if (index < 0 || index >= (int)settings.templates.size())
        return;
      int previous = settings.default_template;
      settings.default_template = index;
      if (save()) {
        publish_templates();
        status("Default template saved for Quick Mirror.");
      } else
        settings.default_template = previous;
    });
    ui->on_edit_template([this](int index) { edit_template(index); });
    ui->on_new_template([this] {
      ui->set_editing_template(-1);
      ui->set_template_name("");
      ui->set_template_arguments("");
      status("Enter a name and scrcpy arguments, then save.");
    });
    ui->on_save_template([this] { save_template(); });
    ui->on_delete_template([this] { delete_template(); });
    ui->on_hide_to_tray([this] { hide(); });
    ui->on_quit_app([this] { quit(); });
    ui->window().on_close_requested([this] {
      if (explorer)
        explorer->detach();
      ui->set_page(0);
      visible = focused = false;
      hidden_at = app::milliseconds();
      return slint::CloseRequestResponse::HideWindow;
    });
    focus_timer.start(
        slint::TimerMode::Repeated, std::chrono::milliseconds(100), [this] {
          // Keep Files visible while selecting or dragging files from Explorer.
          if (!visible || picking || ui->get_pinned() || ui->get_page() == 3 ||
              (explorer && explorer->busy()))
            return;
          if (desktop::app_has_focus())
            focused = true;
          else if (focused || app::milliseconds() - shown_at > 2000)
            hide();
        });
    device_timer.start(slint::TimerMode::Repeated, std::chrono::seconds(5),
                       [this] {
                         if (settings.auto_update && !auto_install_attempted &&
                             ui->get_update_available() && sessions.empty() &&
                             !picking && !ui->get_scanning() && !pending_quick)
                           update(true);
                         if (visible && ui->get_page() == 0 && !installing)
                           refresh(false, true);
                       });
    if (updates_supported)
      update_timer.start(slint::TimerMode::Repeated, std::chrono::hours(6),
                         [this] { update(false); });
    if (scrcpy->detect(settings.scrcpy_path)) {
      path_changed();
      if (!updated && !startup)
        pending_quick = true;
    } else {
      ui->set_page(1);
      if (!startup)
        show();
      status("Choose " + scrcpy_name + " to get started. The app stays in your tray.");
    }
    if (!desktop->tray_available()) {
      // Without a tray host the window is the only way to reach the app.
      log("[tray] No system tray found; keeping the window open.\n");
      ui->set_pinned(true);
      if (startup)
        show();
    }
    if (updated) {
#ifdef _WIN32
      std::ifstream result(app::from_utf8(app::environment("LOCALAPPDATA")) /
                           "scrcpy-pad/updates/result.txt");
#else
      std::ifstream result(update::state_directory() / "result.txt");
#endif
      std::string message((std::istreambuf_iterator<char>(result)),
                          std::istreambuf_iterator<char>());
      log(message);
      status(app::trim(message));
      show();
    }
    // The repository may not have releases yet; this is a normal update state.
    update(false);
  }
};
} // namespace

int main(int argc, char **argv) {
  bool startup = false, updated = false, quick_mirror = false;
  for (int i = 1; i < argc; ++i) {
    startup |= std::string(argv[i]) == "--startup";
    updated |= std::string(argv[i]) == "--updated";
    // Sent by compositor shortcuts on Linux; a first launch mirrors anyway.
    quick_mirror |= std::string(argv[i]) == "--quick-mirror";
  }
  if (!desktop::claim_single_instance(startup, quick_mirror))
    return 0;
  int result = 0;
#ifdef _WIN32
  HRESULT ole = OleInitialize(nullptr);
#else
  // A Slint build with GPU renderers would otherwise load GL drivers (~90 MB
  // RSS); the software renderer is plenty for this panel. Users can override.
  // Must precede any Slint call, since the first one selects the backend.
  setenv("SLINT_BACKEND", "winit-software", 0);
  // Matches scrcpy-gui.desktop and lets compositor rules target the window.
  slint::set_xdg_app_id("scrcpy-gui");
#endif
  try {
    auto app = std::make_shared<Controller>();
#ifdef _WIN32
    app->initialize(updated, startup);
#else
    // Under winit on Wayland, a window shown before the event loop starts
    // never draws its first frame, so initialize from inside the loop.
    std::string failure;
    slint::invoke_from_event_loop([&] {
      try {
        app->initialize(updated, startup);
      } catch (const std::exception &e) {
        failure = e.what();
        slint::quit_event_loop();
      }
    });
#endif
    slint::run_event_loop(slint::EventLoopMode::RunUntilQuit);
    app->desktop.reset();
#ifndef _WIN32
    if (!failure.empty())
      throw std::runtime_error(failure);
#endif
  } catch (const std::exception &e) {
    desktop::show_error(e.what());
    result = 1;
  }
  desktop::release_single_instance();
#ifdef _WIN32
  if (SUCCEEDED(ole))
    OleUninitialize();
#endif
  return result;
}
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return main(__argc, __argv);
}
#endif
