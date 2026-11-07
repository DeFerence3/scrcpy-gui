#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Desktop integration: tray icon, global hotkey, single instance activation,
// window placement and native file dialogs. Implemented in Desktop_win.cpp and
// Desktop_linux.cpp. Every callback runs on the Slint UI thread.
namespace slint { class Window; }

namespace desktop {
// Must match MainWindow's title; Windows locates the native window by it.
inline constexpr char window_title[] = "Scrcpy Tray Tool";

struct Events {
  std::function<void()> activate;        // primary click on the tray icon
  std::function<void()> toggle;          // tray menu "Show / hide"
  std::function<void()> quick_mirror;    // tray menu "Quick Mirror" or hotkey
  std::function<void()> second_instance; // the app was launched again
  std::function<void()> quit;            // tray menu "Quit"
};

class Integration {
public:
  static std::unique_ptr<Integration> create(Events events);
  virtual ~Integration() = default;
  // False when no tray host is reachable, so the window should stay reachable.
  virtual bool tray_available() const = 0;
  virtual bool hotkeys_supported() const = 0;
  // Returns a registration id (> 0), 0 when the combination is unavailable,
  // or -1 when the text is not a valid shortcut.
  virtual int register_hotkey(const std::string &text, std::string &canonical, std::string &error) = 0;
  virtual void unregister_hotkey(int id) = 0;
};

// Returns false when another instance already runs; unless `startup` is set,
// that instance is asked to show itself (or, with `quick_mirror`, to run Quick
// Mirror). Call once, before creating windows.
bool claim_single_instance(bool startup, bool quick_mirror = false);
void release_single_instance();

// Native handle of the main window (HWND on Windows), or nullptr.
void *native_window();
// Restores, positions and focuses the main window after Slint shows it.
void present_window();
// Whether the foreground window belongs to this app (used for auto-hide).
bool app_has_focus();
void show_error(const std::string &message);

struct FileDialog {
  std::string title;
  bool save = false, multiple = false;
  std::string suggested_name;
  std::vector<std::pair<std::string, std::string>> filters; // label, glob
};
// Shows a native file chooser without blocking the UI thread. `done` gets the
// selected paths (empty when cancelled) and an error message on failure.
using FileDialogDone = std::function<void(std::vector<std::filesystem::path>, std::string)>;
void choose_files(const FileDialog &dialog, FileDialogDone done);

#ifndef _WIN32
// File drops onto the main window from file managers (Wayland). Windows uses an
// OLE drop target in FileExplorer instead. Returns nullptr when the window has
// no Wayland surface yet or the compositor offers no data device.
class FileDropTarget {
public:
  virtual ~FileDropTarget() = default;
};
using DropHover = std::function<void(bool)>;
using DropFiles = std::function<void(std::vector<std::filesystem::path>)>;
std::unique_ptr<FileDropTarget> create_file_drop_target(const slint::Window &window, DropHover hover, DropFiles drop);
#endif
}
