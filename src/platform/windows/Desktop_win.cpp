#include "platform/Desktop.h"
#include "platform/Platform.h"
#include <slint.h>
#include <algorithm>
#include <set>
#include <shellapi.h>
#include <shobjidl.h>
#include <thread>

namespace desktop {
namespace {
constexpr UINT TrayMessage = WM_APP + 1;
constexpr UINT ShowCommand = 1001, MirrorCommand = 1002, QuitCommand = 1003;
const wchar_t *MessageClass = L"ScrcpyTrayMsgWnd";
HANDLE instance_mutex = nullptr;

HWND main_window() { return FindWindowW(nullptr, app::wide(window_title).c_str()); }

class WinIntegration : public Integration {
public:
  explicit WinIntegration(Events events) : events_(std::move(events)) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.lpszClassName = MessageClass;
    RegisterClassW(&wc);
    tray_ = CreateWindowExW(0, MessageClass, L"", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    SetWindowLongPtrW(tray_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    icon_.cbSize = sizeof(icon_);
    icon_.hWnd = tray_;
    icon_.uID = 1;
    icon_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    icon_.uCallbackMessage = TrayMessage;
    icon_.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wcscpy_s(icon_.szTip, L"scrcpy-gui · Quick Mirror");
    Shell_NotifyIconW(NIM_ADD, &icon_);
  }
  ~WinIntegration() override {
    Shell_NotifyIconW(NIM_DELETE, &icon_);
    for (int id : hotkeys_) UnregisterHotKey(tray_, id);
    if (tray_) {
      SetWindowLongPtrW(tray_, GWLP_USERDATA, 0);
      DestroyWindow(tray_);
    }
  }
  bool tray_available() const override { return tray_ != nullptr; }
  bool hotkeys_supported() const override { return true; }
  int register_hotkey(const std::string &text, std::string &canonical, std::string &error) override {
    UINT modifiers = 0, key = 0;
    if (!app::parse_hotkey(text, modifiers, key, canonical)) return -1;
    int id = 1;
    while (hotkeys_.count(id)) ++id;
    if (!RegisterHotKey(tray_, id, modifiers | MOD_NOREPEAT, key)) {
      error = app::last_error();
      return 0;
    }
    hotkeys_.insert(id);
    return id;
  }
  void unregister_hotkey(int id) override {
    if (hotkeys_.erase(id)) UnregisterHotKey(tray_, id);
  }

private:
  Events events_;
  HWND tray_ = nullptr;
  NOTIFYICONDATAW icon_{};
  std::set<int> hotkeys_;

  static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto self = reinterpret_cast<WinIntegration *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    static UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbar_created) {
      Shell_NotifyIconW(NIM_ADD, &self->icon_);
      return 0;
    }
    if (msg == TrayMessage) {
      if (lp == WM_LBUTTONUP) self->events_.activate();
      else if (lp == WM_RBUTTONUP) {
        POINT point;
        GetCursorPos(&point);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, MirrorCommand, L"Quick Mirror");
        AppendMenuW(menu, MF_STRING, ShowCommand, L"Show / hide");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, QuitCommand, L"Quit");
        SetForegroundWindow(hwnd);
        UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        if (command == MirrorCommand) self->events_.quick_mirror();
        else if (command == ShowCommand) self->events_.toggle();
        else if (command == QuitCommand) self->events_.quit();
      }
      return 0;
    }
    if (msg == WM_HOTKEY && self->hotkeys_.count((int)wp)) {
      self->events_.quick_mirror();
      return 0;
    }
    if (msg == WM_COPYDATA) {
      self->events_.second_instance();
      return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
};
}

std::unique_ptr<Integration> Integration::create(Events events) {
  return std::make_unique<WinIntegration>(std::move(events));
}

bool claim_single_instance(bool startup, bool) {
  instance_mutex = CreateMutexW(nullptr, FALSE, L"Local\\ScrcpyPadSlint");
  if (GetLastError() != ERROR_ALREADY_EXISTS) return true;
  // Sign-in and installer launches must not activate an existing session.
  if (!startup) {
    if (HWND existing = FindWindowW(MessageClass, nullptr)) {
      COPYDATASTRUCT data{};
      SendMessageW(existing, WM_COPYDATA, 0, (LPARAM)&data);
    }
  }
  release_single_instance();
  return false;
}
void release_single_instance() {
  if (instance_mutex) CloseHandle(instance_mutex);
  instance_mutex = nullptr;
}

void *native_window() { return main_window(); }

void present_window() {
  HWND hwnd = main_window();
  if (!hwnd) return;
  // Restore before measuring: a minimized window has a tiny rectangle, which
  // would otherwise anchor the full window below the work area.
  ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
  RECT area{}, rect{};
  POINT cursor{};
  GetCursorPos(&cursor);
  MONITORINFO monitor{sizeof(monitor)};
  if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor))
    area = monitor.rcWork;
  else
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
  GetWindowRect(hwnd, &rect);
  SetWindowPos(hwnd, HWND_TOPMOST,
               std::max<LONG>(area.left, area.right - (rect.right - rect.left) - 12),
               std::max<LONG>(area.top, area.bottom - (rect.bottom - rect.top) - 12),
               0, 0, SWP_NOSIZE);
  SetForegroundWindow(hwnd);
}

bool app_has_focus() {
  HWND hwnd = main_window(), fg = GetForegroundWindow();
  // Combo-box popups and file pickers owned by this process count as focus.
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return hwnd && (fg == hwnd || pid == GetCurrentProcessId());
}

void show_error(const std::string &message) {
  MessageBoxW(nullptr, app::wide(message).c_str(), L"scrcpy-gui", MB_OK | MB_ICONERROR);
}

void choose_files(const FileDialog &options, FileDialogDone done) {
  HWND owner = main_window();
  // The dialog runs modally on its own STA thread so the Slint loop keeps running.
  std::thread([options, owner, done = std::move(done)] {
    std::vector<std::filesystem::path> selected;
    std::string error;
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(result)) {
      IFileDialog *dialog = nullptr;
      result = CoCreateInstance(options.save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
      if (SUCCEEDED(result)) {
        std::vector<std::wstring> strings;
        for (const auto &[label, glob] : options.filters) { strings.push_back(app::wide(label)); strings.push_back(app::wide(glob)); }
        std::vector<COMDLG_FILTERSPEC> filters;
        for (size_t i = 0; i < strings.size(); i += 2) filters.push_back({strings[i].c_str(), strings[i + 1].c_str()});
        if (!filters.empty()) dialog->SetFileTypes((UINT)filters.size(), filters.data());
        dialog->SetTitle(app::wide(options.title).c_str());
        if (!options.suggested_name.empty()) dialog->SetFileName(app::wide(options.suggested_name).c_str());
        FILEOPENDIALOGOPTIONS flags = FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
        flags |= options.save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST;
        if (options.multiple && !options.save) flags |= FOS_ALLOWMULTISELECT;
        dialog->SetOptions(flags);
        result = dialog->Show(owner);
        auto add = [&](IShellItem *item) {
          PWSTR path = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { selected.emplace_back(path); CoTaskMemFree(path); }
        };
        if (SUCCEEDED(result) && options.save) {
          IShellItem *item = nullptr;
          if (SUCCEEDED(dialog->GetResult(&item))) { add(item); item->Release(); }
        } else if (SUCCEEDED(result)) {
          IFileOpenDialog *open = nullptr;
          IShellItemArray *items = nullptr;
          if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&open))) && SUCCEEDED(open->GetResults(&items))) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
              IShellItem *item = nullptr;
              if (SUCCEEDED(items->GetItemAt(i, &item))) { add(item); item->Release(); }
            }
            items->Release();
          }
          if (open) open->Release();
        }
        dialog->Release();
      }
      CoUninitialize();
    }
    if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_CANCELLED))
      error = "Could not open the file dialog.";
    slint::invoke_from_event_loop([done, selected = std::move(selected), error] { done(selected, error); });
  }).detach();
}
}
