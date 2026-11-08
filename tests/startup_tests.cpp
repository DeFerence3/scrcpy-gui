#include "platform/Platform.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Child {
  PROCESS_INFORMATION process{};
  explicit Child(const std::filesystem::path &exe) {
    STARTUPINFOW startup{sizeof(startup)};
    auto command = app::quote_arg(exe.wstring()) + L" --startup";
    require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(),
                           &startup, &process), "Could not launch startup test");
    CloseHandle(process.hThread);
  }
  ~Child() {
    // Only terminate the process created by this test, never a user's tray app.
    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess);
  }
};
struct Windows {
  DWORD pid;
  bool visible = false;
};
BOOL CALLBACK inspect_window(HWND window, LPARAM data) {
  auto &state = *reinterpret_cast<Windows *>(data);
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  wchar_t type[256]{};
  GetClassNameW(window, type, 256);
  // Winit's transparent event target uses WS_VISIBLE to receive WM_PAINT;
  // it is an internal message window, not an app popup.
  if (pid == state.pid && IsWindowVisible(window) &&
      std::wstring(type) != L"Winit Thread Event Target") {
    state.visible = true;
  }
  return TRUE;
}
bool visible(DWORD pid) {
  Windows state{pid};
  EnumWindows(inspect_window, reinterpret_cast<LPARAM>(&state));
  return state.visible;
}
void require_hidden(const Child &child, DWORD duration) {
  auto deadline = GetTickCount64() + duration;
  do {
    require(WaitForSingleObject(child.process.hProcess, 0) == WAIT_TIMEOUT,
            "Startup process exited unexpectedly");
    require(!visible(child.process.dwProcessId), "Startup opened a window");
    Sleep(25);
  } while (GetTickCount64() < deadline);
}
HWND wait_for_tray(const Child &child) {
  bool ready = false;
  HWND tray = nullptr;
  for (int attempt = 0; attempt < 200 && !ready; ++attempt) {
    require_hidden(child, 25);
    DWORD pid = 0;
    tray = FindWindowW(L"ScrcpyTrayMsgWnd", nullptr);
    GetWindowThreadProcessId(tray, &pid);
    ready = pid == child.process.dwProcessId;
  }
  require(ready, "Startup did not create its tray window");
  return tray;
}
bool wait_for(const std::function<bool()> &condition) {
  for (int attempt = 0; attempt < 120; ++attempt) {
    if (condition())
      return true;
    Sleep(25);
  }
  return false;
}
std::string read(const std::filesystem::path &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input), {});
}
HWND app_window(const Child &child) {
  auto window = FindWindowW(nullptr, L"Scrcpy Tray Tool");
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  return pid == child.process.dwProcessId ? window : nullptr;
}
bool fully_visible(const Child &child) {
  auto window = app_window(child);
  if (!window || !IsWindowVisible(window) || IsIconic(window))
    return false;
  RECT client{}, bounds{};
  if (!GetClientRect(window, &client) || !GetWindowRect(window, &bounds))
    return false;
  const auto dpi = GetDpiForWindow(window);
  if (client.right < MulDiv(380, dpi, 96) || client.bottom < MulDiv(660, dpi, 96))
    return false;
  MONITORINFO monitor{sizeof(monitor)};
  return GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
         bounds.left >= monitor.rcWork.left && bounds.top >= monitor.rcWork.top &&
         bounds.right <= monitor.rcWork.right && bounds.bottom <= monitor.rcWork.bottom;
}
void startup_case(const std::filesystem::path &exe) {
  Child child(exe);
  wait_for_tray(child);
  require_hidden(child, 2000);
  auto duplicate = app::run_process(app::to_utf8(exe), {"--startup"}, {}, 3000);
  require(duplicate.code == 0 && !duplicate.timed_out,
          "Duplicate startup did not exit successfully");
  require_hidden(child, 1000);

  // Manual activation must still work after a quiet sign-in launch.
  auto manual = app::run_process(app::to_utf8(exe), {}, {}, 3000);
  require(manual.code == 0 && !manual.timed_out, "Manual activation failed");
  bool shown = false;
  for (int attempt = 0; attempt < 80 && !shown; ++attempt) {
    shown = visible(child.process.dwProcessId);
    Sleep(25);
  }
  require(shown, "Manual activation did not open the running GUI");
}
void shortcut_case(const std::filesystem::path &exe, const std::string &devices,
                   bool should_mirror) {
  const auto listing = exe.parent_path() / "fixture-devices.txt";
  const auto launches = exe.parent_path() / "fixture-mirrors.txt";
  std::ofstream(listing) << "List of devices attached\n" << devices;
  std::ofstream(launches).close();
  Child child(exe);
  auto tray = wait_for_tray(child);
  // Deliver the same message Windows sends for the registered shortcut.
  auto shortcut = [&] {
    require(PostMessageW(tray, WM_HOTKEY, 1, MAKELPARAM(MOD_CONTROL | MOD_ALT, 'S')),
            "Could not deliver the shortcut to the test app");
  };
  shortcut();
  if (should_mirror) {
    require(wait_for([&] { return !read(launches).empty(); }),
            "Shortcut did not start the single ready device");
    require_hidden(child, 250);
    require(read(launches) == "--serial=USB123\n", "Shortcut started the wrong device");
    shortcut();
    require(wait_for([&] { return visible(child.process.dwProcessId); }),
            "Shortcut did not show the app while a device was mirroring");

    // Even if only a different device is ready now, an existing session wins.
    auto window = FindWindowW(nullptr, L"Scrcpy Tray Tool");
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    require(pid == child.process.dwProcessId, "Could not find the test app window");
    PostMessageW(window, WM_CLOSE, 0, 0);
    require(wait_for([&] { return !visible(child.process.dwProcessId); }),
            "Could not hide the test app in its tray");
    std::ofstream(listing) << "List of devices attached\n192.168.1.15:5555 device model:Galaxy_S23\n";
    shortcut();
    require(wait_for([&] { return visible(child.process.dwProcessId); }),
            "Shortcut started a different device instead of showing the active session");
  } else {
    require(wait_for([&] { return visible(child.process.dwProcessId); }),
            "Shortcut did not show the app when no single device was ready");
  }
  // Repeating the shortcut must keep the app visible, not toggle it closed.
  shortcut();
  Sleep(250);
  require(visible(child.process.dwProcessId), "Repeated shortcut hid the app");
  require(read(launches) == (should_mirror ? "--serial=USB123\n" : ""),
          "Shortcut launched an unwanted mirror session");
  require(wait_for([&] { return fully_visible(child); }),
          "Shortcut showed a collapsed or off-screen window");

  // Minimize the test window as Windows does for Show Desktop, then restore it
  // through the actual shortcut handler instead of accepting WS_VISIBLE alone.
  auto window = app_window(child);
  PostMessageW(window, WM_SYSCOMMAND, SC_MINIMIZE, 0);
  require(wait_for([&] { return IsIconic(window) != FALSE; }),
          "Could not minimize the test app");
  shortcut();
  require(wait_for([&] { return fully_visible(child); }),
          "Shortcut did not restore the minimized app at full size inside the screen");
  require(read(launches) == (should_mirror ? "--serial=USB123\n" : ""),
          "Restoring the minimized app launched an unwanted mirror session");
}
} // namespace

int main(int argc, char **argv) {
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (FindWindowW(L"ScrcpyTrayMsgWnd", nullptr)) {
    std::cout << "Quit the running tray app to test automatic startup.\n";
    return 77;
  }
  try {
    require(argc == 4, "Expected GUI, fixture and work directory");
    const auto source = std::filesystem::absolute(app::wide(argv[1]));
    const auto fixture = std::filesystem::absolute(app::wide(argv[2]));
    auto root = std::filesystem::absolute(app::wide(argv[3])) /
                ("case with spaces " + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    auto exe = root / "scrcpy-ui.exe";
    std::filesystem::copy_file(source, exe);
    for (const auto &entry : std::filesystem::directory_iterator(source.parent_path()))
      if (entry.path().extension() == L".dll")
        std::filesystem::copy_file(entry.path(), root / entry.path().filename());

    // Child-only test environment: no saved configuration, real phone tools,
    // updater script/network traffic, or changes to the user's startup entry.
    SetEnvironmentVariableW(L"APPDATA", (root / "settings").c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA", (root / "local").c_str());
    SetEnvironmentVariableW(L"SCRCPY", nullptr);
    SetEnvironmentVariableW(L"PATH", L"");
    require(SetCurrentDirectoryW(root.c_str()), "Could not isolate test directory");
    startup_case(exe);
    std::cout << "PASS: unconfigured startup stays hidden and activates manually\n";

    std::filesystem::copy_file(fixture, root / "scrcpy.exe");
    std::filesystem::copy_file(fixture, root / "adb.exe");
    SetEnvironmentVariableW(L"SCRCPY", (root / "scrcpy.exe").c_str());
    startup_case(exe);
    std::cout << "PASS: configured startup and duplicate stay hidden without mirroring\n";
    shortcut_case(exe, "", false);
    std::cout << "PASS: shortcut shows the app with no devices\n";
    shortcut_case(exe, "LOCK unauthorized\nOFFLINE offline\n", false);
    std::cout << "PASS: shortcut shows the app with no ready devices\n";
    shortcut_case(exe, "USB123 device model:Pixel_7_Pro\n192.168.1.15:5555 device model:Galaxy_S23\n", false);
    std::cout << "PASS: shortcut shows device selection with multiple ready devices\n";
    shortcut_case(exe, "USB123 device model:Pixel_7_Pro\n", true);
    std::cout << "PASS: shortcut starts one ready device, then shows the app while mirroring\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
