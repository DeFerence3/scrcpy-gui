#include "platform/Platform.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
void wait_seconds(int seconds) { std::this_thread::sleep_for(std::chrono::seconds(seconds)); }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_args) {
  std::vector<std::string> argv;
  for (int i = 0; i < argc; ++i) argv.push_back(app::utf8(wide_args[i]));
#else
int main(int argc, char **raw_args) {
  std::vector<std::string> argv(raw_args, raw_args + argc);
#endif
  auto arg_path = [&](int i) { return app::from_utf8(argv[i]); };
  std::string first = argc > 1 ? argv[1] : "";
  if (first == "-s" && argc >= 4) {
    if (argv[2] != "USB123" && argv[2] != "192.168.1.15:5555") { std::cerr << "device offline"; return 1; }
    if (argv[3] == "shell" && argc == 6 && argv[4] == "-T") {
      const auto &script = argv[5];
      if (script.find("cd '/denied'") == 0) { std::cerr << "Permission denied"; return 1; }
      if (script.find("cd '/slow'") == 0) wait_seconds(10);
      if (script.find("cd '/sdcard/space '\\'' & 雪'") != 0 &&
          script.find("cd '/sdcard'") != 0 && script.find("cd '/empty'") != 0 &&
          script.find("cd '/broken'") != 0 && script.find("cd '/slow'") != 0) {
        std::cerr << "Unexpected or unescaped remote path: " << script; return 1;
      }
#ifdef _WIN32
      _setmode(_fileno(stdout), _O_BINARY);
#endif
      auto field = [](const std::string &value) { std::cout.write(value.data(), value.size()); std::cout.put('\0'); };
      field("SCRCPY_FILES");
      if (script.find("cd '/broken'") == 0) { field("f"); return 0; }
      if (script.find("cd '/empty'") != 0) {
        field("f"); field("1234"); field("quote ' & 雪\nfile.txt");
        field("d"); field("0"); field("Download");
        field("f"); field("0"); field(".hidden");
      }
      field("SCRCPY_END");
    } else if (argv[3] == "pull" && argc == 6) {
      std::ofstream output(arg_path(5), std::ios::binary);
      output << "fixture download";
      output.close();
      if (argv[4] == "/fail.bin") { std::cerr << "device disconnected"; return 1; }
      if (argv[4] == "/slow.bin") wait_seconds(10);
      if (argv[4] != "/sdcard/quote ' & 雪\nfile.txt" && argv[4] != "/slow.bin") { std::cerr << "Unexpected pull path"; return 1; }
      if (!output) return 1;
    } else if (argv[3] == "push" && argc == 6) {
      if (argv[5] != "/sdcard/Download/" && argv[5] != "/sdcard/space ' & 雪/") { std::cerr << "Wrong upload folder: " << argv[5]; return 1; }
      auto source = arg_path(4);
      if (!std::filesystem::is_regular_file(source)) return 1;
      if (source.filename() == "fail.txt") { std::cerr << "remote permission denied"; return 1; }
      auto destination = app::executable_directory() / "uploaded";
      std::filesystem::create_directories(destination);
      std::filesystem::copy_file(source, destination / source.filename(), std::filesystem::copy_options::overwrite_existing);
      std::ofstream(destination / "destination.txt", std::ios::binary) << argv[2] << '\n' << argv[5];
    } else { std::cerr << "Invalid ADB file arguments"; return 1; }
  } else if (first == "devices") {
    std::ifstream devices(app::executable_directory() / "fixture-devices.txt");
    if (devices)
      std::cout << devices.rdbuf();
    else
      std::cout << "List of devices attached\nUSB123 device model:Pixel_7_Pro\n192.168.1.15:5555 device model:Galaxy_S23\nLOCK unauthorized\n";
  } else if (first == "--version") std::cout << "scrcpy fixture\n";
  else if (first == "--updated") std::ofstream("restarted.txt") << "updated";
  else if (first == "--startup") std::ofstream("started.txt") << "--startup";
#ifdef _WIN32
  else if (first == "--tray") {
    WNDCLASSW window{};
    window.lpfnWndProc = DefWindowProcW;
    window.hInstance = GetModuleHandleW(nullptr);
    window.lpszClassName = L"ScrcpyTrayMsgWnd";
    RegisterClassW(&window);
    auto tray = CreateWindowW(window.lpszClassName, L"", 0, 0, 0, 0, 0,
                              nullptr, nullptr, window.hInstance, nullptr);
    if (!tray) return 1;
    Sleep(30000);
    DestroyWindow(tray);
  }
#endif
  else if (first.rfind("--serial=", 0) == 0) {
    std::ofstream(app::executable_directory() / "fixture-mirrors.txt", std::ios::app) << first << '\n';
    for (int i=1; i<argc; ++i) std::cout << argv[i] << '\n';
    std::cout.flush();
    wait_seconds(10);
  }
  return 0;
}
