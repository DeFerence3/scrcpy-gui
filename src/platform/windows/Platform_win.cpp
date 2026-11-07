#include "platform/Platform.h"
#include "core/AppCore.h"
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace app {
namespace {
struct Handle {
  HANDLE value = nullptr;
  explicit Handle(HANDLE h = nullptr): value(h) {}
  ~Handle() { close(); }
  void close() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
};
std::wstring command_line(const std::wstring &exe, const std::vector<std::string> &args) {
  auto line = quote_arg(exe);
  for (const auto &arg : args) line += L" " + quote_arg(wide(arg));
  return line;
}
std::string system_error(DWORD code) {
  wchar_t buffer[1024]{};
  FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buffer, 1024, nullptr);
  return trim(utf8(buffer)) + " (" + std::to_string(code) + ")";
}
}
std::wstring wide(const std::string &value) {
  int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0);
  std::wstring result(count, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), count);
  return result;
}
std::string utf8(const std::wstring &value) {
  int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
  std::string result(count, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(), result.data(), count, nullptr, nullptr);
  return result;
}
std::string last_error() { return system_error(GetLastError()); }
unsigned long process_id() { return GetCurrentProcessId(); }
std::wstring quote_arg(const std::wstring &value) {
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (auto c : value) {
    if (c == L'\\') { ++backslashes; continue; }
    out.append(backslashes * (c == L'"' ? 2 : 1), L'\\');
    backslashes = 0;
    if (c == L'"') out += L'\\';
    out += c;
  }
  out.append(backslashes * 2, L'\\');
  return out + L'"';
}
std::string environment(const char *name) {
  std::vector<wchar_t> value(32768);
  DWORD size = GetEnvironmentVariableW(wide(name).c_str(), value.data(), (DWORD)value.size());
  return size && size < value.size() ? utf8(value.data()) : std::string{};
}
std::filesystem::path config_directory() {
  auto dir = environment("APPDATA");
  if (dir.empty()) throw std::runtime_error("APPDATA is unavailable.");
  return from_utf8(dir) / "scrcpy-tray";
}
std::filesystem::path executable_directory() {
  std::vector<wchar_t> path(32768);
  GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
  return std::filesystem::path(path.data()).parent_path();
}
bool is_file(const std::filesystem::path &path) {
  DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}
std::filesystem::path find_executable(const std::string &name) {
  std::vector<wchar_t> buffer(32768);
  DWORD count = SearchPathW(nullptr, wide(name).c_str(), L".exe", (DWORD)buffer.size(), buffer.data(), nullptr);
  return count && count < buffer.size() ? std::filesystem::path(buffer.data()) : std::filesystem::path{};
}
std::string create_new_file(const std::filesystem::path &path, bool &exists) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  exists = file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS;
  if (file == INVALID_HANDLE_VALUE) return exists ? "File exists." : last_error();
  CloseHandle(file);
  return {};
}
std::string replace_file(const std::filesystem::path &from, const std::filesystem::path &to) {
  if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return {};
  return last_error();
}
bool open_url(const std::string &url, std::string &error) {
  auto result = ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) > 32) return true;
  error = "ShellExecute failed.";
  return false;
}
bool parse_hotkey(const std::string &text, UINT &modifiers, UINT &key, std::string &canonical) {
  modifiers = 0; key = 0; canonical.clear();
  std::istringstream parts(text);
  std::string part;
  while (std::getline(parts, part, '+')) {
    part = trim(part);
    std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    UINT modifier = part == "CTRL" || part == "CONTROL" ? MOD_CONTROL : part == "ALT" ? MOD_ALT
      : part == "SHIFT" ? MOD_SHIFT : part == "WIN" ? MOD_WIN : 0;
    if (modifier) { if (modifiers & modifier) return false; modifiers |= modifier; continue; }
    if (key) return false;
    if (part.size() == 1 && ((part[0] >= 'A' && part[0] <= 'Z') || (part[0] >= '0' && part[0] <= '9'))) key = part[0];
    else if (part.size() >= 2 && part[0] == 'F' && part.substr(1).find_first_not_of("0123456789") == std::string::npos) {
      if (part.size() > 3) return false;
      int f = std::stoi(part.substr(1));
      if (f < 1 || f > 24) return false;
      key = VK_F1 + f - 1;
    } else return false;
  }
  if (!key || !modifiers || (!text.empty() && text.back() == '+')) return false;
  if (modifiers & MOD_CONTROL) canonical += "Ctrl + ";
  if (modifiers & MOD_ALT) canonical += "Alt + ";
  if (modifiers & MOD_SHIFT) canonical += "Shift + ";
  if (modifiers & MOD_WIN) canonical += "Win + ";
  canonical += key >= VK_F1 && key <= VK_F24 ? "F" + std::to_string(key - VK_F1 + 1) : std::string(1, (char)key);
  return true;
}
ProcessResult run_process(const std::string &executable, const std::vector<std::string> &args,
                         Output output, unsigned timeout_ms, std::shared_ptr<Cancel> cancel,
                         const std::filesystem::path &directory) {
  ProcessResult result;
  auto exe = wide(executable);
  auto fail = [&](const std::string &message) { result.text += message + "\n"; if (output) output(message + "\n"); };
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE read = nullptr, write = nullptr;
  if (!CreatePipe(&read, &write, &security, 0)) { fail(last_error()); return result; }
  Handle reader(read), writer(write);
  if (!SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0)) { fail(last_error()); return result; }
  Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
  if (input.value == INVALID_HANDLE_VALUE) { fail(last_error()); return result; }
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<unsigned char> storage(bytes);
  auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) { fail(last_error()); return result; }
  HANDLE inherited[] = {writer.value, input.value};
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
    fail(last_error()); DeleteProcThreadAttributeList(attributes); return result;
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = writer.value;
  startup.StartupInfo.hStdInput = input.value;
  startup.lpAttributeList = attributes;
  Handle job(CreateJobObjectW(nullptr, nullptr));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  // adb may start its shared daemon. Keep that child outside our job so stopping
  // a mirror (or completing `adb devices`) cannot kill other apps' ADB sessions.
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
  if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    fail(last_error()); DeleteProcThreadAttributeList(attributes); return result;
  }
  PROCESS_INFORMATION info{};
  auto line = command_line(exe, args);
  auto dir = directory.wstring();
  // CreateProcess (never cmd.exe): template arguments cannot execute shell commands.
  BOOL started = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE,
    CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
    dir.empty() ? nullptr : dir.c_str(), &startup.StartupInfo, &info);
  DWORD launch_error = GetLastError();
  DeleteProcThreadAttributeList(attributes);
  if (!started) { fail("Could not start " + executable + ": " + system_error(launch_error)); return result; }
  Handle process(info.hProcess), thread(info.hThread);
  if (!AssignProcessToJobObject(job.value, process.value)) {
    fail(last_error()); TerminateProcess(process.value, 1); return result;
  }
  if (ResumeThread(thread.value) == (DWORD)-1) { fail(last_error()); TerminateProcess(process.value, 1); return result; }
  writer.close(); input.close();
  auto began = GetTickCount64();
  std::string pending;
  bool terminated = false;
  for (;;) {
    if (!terminated && ((cancel && cancel->requested) || (timeout_ms && GetTickCount64() - began > timeout_ms))) {
      result.timed_out = !(cancel && cancel->requested);
      TerminateJobObject(job.value, ERROR_CANCELLED);
      terminated = true;
      if (result.timed_out) fail("Command timed out.");
    }
    DWORD available = 0;
    if (PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr) && available) {
      char buffer[4096]; DWORD count = 0;
      if (ReadFile(reader.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) && count) {
        if (result.text.size() < 1024*1024) result.text.append(buffer, count);
        pending.append(buffer, count);
        auto end = pending.find_last_of('\n');
        if (end != std::string::npos) { if (output) output(pending.substr(0, end+1)); pending.erase(0, end+1); }
        // Also bound output from tools that never write a newline. Keep the last
        // few bytes together so a UTF-8 character is not split across callbacks.
        if (pending.size() > 8192) {
          size_t end = pending.size() - 4;
          while (end && (static_cast<unsigned char>(pending[end]) & 0xc0) == 0x80) --end;
          if (output) output(pending.substr(0, end));
          pending.erase(0, end);
        }
        continue;
      }
    }
    if (WaitForSingleObject(process.value, 20) == WAIT_OBJECT_0) {
      if (!PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr) || !available) break;
    }
  }
  if (output && !pending.empty()) output(pending + "\n");
  DWORD code = 1;
  GetExitCodeProcess(process.value, &code);
  result.code = (int)code;
  return result;
}
bool launch_detached(const std::string &executable, const std::vector<std::string> &args, std::string &error) {
  auto exe = wide(executable);
  auto line = command_line(exe, args);
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
    error = last_error(); return false;
  }
  CloseHandle(info.hProcess); CloseHandle(info.hThread); return true;
}
}
