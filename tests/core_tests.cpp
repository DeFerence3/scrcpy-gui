#include "core/AppCore.h"
#include "core/Json.h"
#include "platform/Platform.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <shellapi.h>
#else
#include "platform/linux/Updater.h"
#include <sys/stat.h>
#include <unistd.h>
#endif

void require(bool test, const char *message) {
  if (!test) throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  // A harmless child process used to verify real CreateProcess quoting and cancellation.
  if (argc > 1 && std::string(argv[1]) == "--echo") {
    for (int i=2; i<argc; ++i) std::cout << argv[i] << '\n';
    return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--wait") { std::this_thread::sleep_for(std::chrono::seconds(10)); return 0; }
  try {
    auto devices = app::parse_devices("* daemon started successfully *\r\nList of devices attached\r\nABC device product:foo model:Pixel_7_Pro transport_id:1\r\n192.168.1.4:5555 device model:Galaxy_S23\r\nLOCK unauthorized\r\nGONE offline\r\nABC device\r\n");
    require(devices.size() == 4, "ADB states or deduplication failed");
    require(devices[0].name == "Pixel 7 Pro" && devices[0].ready(), "ADB model parsing failed");
    require(devices[1].connection == "Wireless" && !devices[2].ready() && !devices[3].ready(), "Device eligibility failed");
    require(app::parse_devices("error: device not found\n").empty(), "Error parsed as device");
#ifdef _WIN32
    UINT modifiers, key; std::string canonical;
    require(app::parse_hotkey("shift + Ctrl + f12", modifiers, key, canonical) && key == VK_F12 && canonical == "Ctrl + Shift + F12", "Shortcut parsing failed");
    for (auto invalid : {"S", "Ctrl +", "Ctrl + S + T", "Ctrl + Ctrl + S", "Alt + F25", "Win + F999999999"})
      require(!app::parse_hotkey(invalid, modifiers, key, canonical), "Invalid shortcut accepted");
#endif
    std::string error;
    auto args = app::template_args("--record=\"C:\\My Videos\\demo.mp4\" --no-audio", error);
    require(error.empty() && args.size() == 2 && args[0] == "--record=C:\\My Videos\\demo.mp4", "Template quoting failed");
    for (auto invalid : {"--serial=other", "-sother", "--select-usb", "--tcpip=host", "--record=\"missing"}) {
      error.clear(); app::template_args(invalid, error); require(!error.empty(), "Unsafe device override accepted");
    }
    // The portable splitter must follow the Windows argv rules exactly.
    using Split = std::pair<const char *, std::vector<std::string>>;
    for (const auto &[line, expected] : std::vector<Split>{
           {"a b\tc", {"a", "b", "c"}},
           {"\"\" x", {"", "x"}},
           {"a\\\\b", {"a\\\\b"}},
           {"\"C:\\dir\\\\\" next", {"C:\\dir\\", "next"}},
           {"say\\\"hi\\\"", {"say\"hi\""}},
           {"\"in \"\"quotes\"\"\"", {"in \"quotes\""}},
           {"  ", {}}}) {
      require(app::split_args(line) == expected, "Argument splitting differs from Windows rules");
#ifdef _WIN32
      int native_count = 0;
      auto native = CommandLineToArgvW((L"scrcpy " + app::wide(line)).c_str(), &native_count);
      std::vector<std::string> native_args;
      for (int i = 1; native && i < native_count; ++i) native_args.push_back(app::utf8(native[i]));
      LocalFree(native);
      require(native_args == expected, "CommandLineToArgvW disagrees with the portable splitter");
#endif
    }
#ifdef _WIN32
    std::vector<std::wstring> values{L"", L"simple", L"C:\\path with spaces\\", L"has\"quote", L"& echo should-not-run"};
    std::wstring line = L"test";
    for (const auto &v : values) line += L" " + app::quote_arg(v);
    int count; auto parsed = CommandLineToArgvW(line.c_str(), &count);
    require(count == (int)values.size()+1, "Quoted argument count wrong");
    for (int i=1; i<count; ++i) require(parsed[i] == values[i-1], "Quoted argument round trip failed");
    LocalFree(parsed);
#endif

    {
      app::json::Value doc;
      require(app::json::parse(R"({"a":[1,-2.5e1,true,null],"s":"x\"\u00e9\ud83d\ude00\n","o":{}})", doc), "JSON parse failed");
      require(doc.get("a")->items.size() == 4 && doc.get("a")->items[1].num == -25 && doc.get("a")->items[2].flag,
              "JSON values wrong");
      require(doc.str("s") == "x\"\xc3\xa9\xf0\x9f\x98\x80\n" && doc.get("o")->type == app::json::Value::object,
              "JSON strings wrong");
      for (auto bad : {"", "{", "[1,]", "{\"a\" 1}", "\"\\ud800\"", "tru", "[1] 2", "\"a\nb\""})
        require(!app::json::parse(bad, doc), "Malformed JSON accepted");
      require(!app::json::parse(std::string(100, '[') + std::string(100, ']'), doc), "Deep JSON accepted");
    }
#ifndef _WIN32
    {
      require(update::compare_versions("1.2.10", "1.2.9") > 0 && update::compare_versions("1.2.0", "1.2.0.0") == 0 &&
              update::compare_versions("0.9.9", "1.0.0") < 0, "Version comparison failed");
      for (auto bad : {"1.2", "v1.2.3", "1..2.3", "1.2.3.4.5", "1.2.x"})
        require(!update::valid_version(bad), "Invalid version accepted");
      const std::string repo = "owner/repo", asset = update::asset_name();
      auto release = [&](const std::string &tag, const std::string &url, const std::string &digest) {
        return "{\"tag_name\":\"" + tag + "\",\"assets\":[{\"name\":\"other.zip\"},{\"name\":\"" + asset +
               "\",\"browser_download_url\":\"" + url + "\",\"digest\":\"" + digest + "\",\"size\":1234}]}";
      };
      const std::string good_url = "https://github.com/owner/repo/releases/download/v1.4.0/" + asset;
      const std::string digest = "sha256:" + std::string(64, 'A');
      std::string version;
      update::Asset found;
      require(update::parse_release(release("v1.4.0", good_url, digest), repo, asset, version, found).empty() &&
              version == "1.4.0" && found.sha256 == std::string(64, 'a') && found.size == 1234, "Release parsing failed");
      require(!update::parse_release(release("v1.4.0", "https://github.com/evil/repo/releases/download/x", digest), repo, asset, version, found).empty(),
              "Foreign download URL accepted");
      require(!update::parse_release(release("v1.4.0", good_url, "sha1:abc"), repo, asset, version, found).empty(),
              "Missing digest accepted");
      require(!update::parse_release(release("latest", good_url, digest), repo, asset, version, found).empty(),
              "Invalid tag accepted");
      for (auto ok : {"bin/scrcpy-ui", "lib/libslint_cpp.so", "lib/libslint_cpp.so.1", "share/doc/scrcpy-gui/README.md",
                      "share/applications/scrcpy-gui.desktop"})
        require(update::allowed_entry(ok), "Release file rejected");
      for (auto bad : {"/bin/scrcpy-ui", "bin/../../.bashrc", "bin/sh", "lib/libslint_cpp.so/x", "share/doc/scrcpy-gui/a/b",
                       "share/applications/evil.desktop"})
        require(!update::allowed_entry(bad), "Unexpected release file accepted");

      // Install a staged release over an existing prefix.
      auto root = std::filesystem::temp_directory_path() / ("scrcpy-update-test-" + std::to_string(app::process_id()));
      std::filesystem::remove_all(root);
      auto stage = root / "stage", prefix = root / "prefix";
      auto write = [](const std::filesystem::path &path, const std::string &text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
      };
      write(stage / "files/bin/scrcpy-ui", "new-exe");
      chmod((stage / "files/bin/scrcpy-ui").c_str(), 0755);
      write(stage / "files/lib/libslint_cpp.so", "new-lib");
      write(stage / "files/share/doc/scrcpy-gui/README.md", "docs");
      write(prefix / "bin/scrcpy-ui", "old-exe");
      write(prefix / "lib/libslint_cpp.so", "old-lib");
      auto read = [](const std::filesystem::path &path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      };
      require(update::apply(stage, prefix).empty(), "Update install failed");
      require(read(prefix / "bin/scrcpy-ui") == "new-exe" && read(prefix / "lib/libslint_cpp.so") == "new-lib" &&
              read(prefix / "share/doc/scrcpy-gui/README.md") == "docs", "Update files not installed");
      require(access((prefix / "bin/scrcpy-ui").c_str(), X_OK) == 0, "Update lost the executable bit");
      require(!std::filesystem::exists(prefix / "bin/scrcpy-ui.scrcpy-old") && !std::filesystem::exists(stage),
              "Update left backups or the stage behind");
      // A staged file outside the allowlist aborts before anything changes.
      write(stage / "files/bin/scrcpy-ui", "newer-exe");
      write(stage / "files/bin/evil", "x");
      require(!update::apply(stage, prefix).empty() && read(prefix / "bin/scrcpy-ui") == "new-exe",
              "Unexpected staged file was installed");
      // Release archives: plain allowed files extract; links are refused.
      auto tree = root / "tree";
      write(tree / "bin/scrcpy-ui", "exe");
      write(tree / "lib/libslint_cpp.so", "lib");
      auto archive = app::to_utf8(root / "good.tar.gz");
      require(app::run_process("tar", {"-czf", archive, "-C", app::to_utf8(tree), "bin", "lib"}).code == 0, "tar failed");
      require(update::extract(archive, root / "out").empty() && read(root / "out/bin/scrcpy-ui") == "exe",
              "Valid release archive rejected");
      std::filesystem::create_symlink("/etc/passwd", tree / "lib/libslint_cpp.so.1");
      archive = app::to_utf8(root / "link.tar.gz");
      require(app::run_process("tar", {"-czf", archive, "-C", app::to_utf8(tree), "bin", "lib"}).code == 0, "tar failed");
      require(!update::extract(archive, root / "out2").empty() && !std::filesystem::exists(root / "out2"),
              "Release archive with a symlink accepted");
      std::filesystem::remove_all(tree / "lib/libslint_cpp.so.1");
      write(tree / "etc/evil", "x");
      archive = app::to_utf8(root / "extra.tar.gz");
      require(app::run_process("tar", {"-czf", archive, "-C", app::to_utf8(tree), "bin", "etc"}).code == 0, "tar failed");
      require(!update::extract(archive, root / "out3").empty(), "Release archive with extra files accepted");
      std::filesystem::remove_all(root);
    }
#endif
    auto directory = std::filesystem::temp_directory_path() / ("scrcpy-pad-test-" + std::to_string(app::process_id()));
    auto path = directory / "settings.dat";
    app::Settings settings; settings.scrcpy_path = "C:\\测试 path\\scrcpy.exe";
    settings.shortcut = "Ctrl + Shift + F8";
    settings.templates.push_back({"My \"quoted\" template", "--record=\"C:\\videos\\record.mp4\""});
    settings.default_template = 3; settings.auto_update = true;
    error.clear(); require(app::save_settings(path, settings, error), "Settings save failed");
    app::Settings loaded; require(app::load_settings(path, loaded, error), "Settings load failed");
    auto require_settings_preserved = [&] {
      require(loaded.scrcpy_path == settings.scrcpy_path && loaded.shortcut == settings.shortcut &&
              loaded.default_template == settings.default_template && loaded.auto_update == settings.auto_update &&
              loaded.templates.size() == settings.templates.size(), "Settings round trip failed");
      for (size_t i = 0; i < settings.templates.size(); ++i)
        require(loaded.templates[i].name == settings.templates[i].name &&
                loaded.templates[i].arguments == settings.templates[i].arguments, "Saved template changed");
    };
    require_settings_preserved();
    {
      std::ofstream legacy(path, std::ios::trunc);
      legacy << "SCRCPY_PAD 1\n" << std::quoted(settings.scrcpy_path) << '\n'
             << std::quoted(settings.shortcut) << "\n\"custom-owner/old-updates\"\n"
             << settings.auto_update << ' ' << settings.default_template << ' ' << settings.templates.size() << '\n';
      for (const auto &t : settings.templates)
        legacy << std::quoted(t.name) << ' ' << std::quoted(t.arguments) << '\n';
    }
    loaded = app::Settings{};
    require(app::load_settings(path, loaded, error), "Legacy settings load failed");
    require_settings_preserved();
    require(app::save_settings(path, loaded, error), "Legacy settings migration failed");
    {
      std::ifstream migrated(path);
      std::string header;
      std::getline(migrated, header);
      require(header == "SCRCPY_PAD 2", "Settings were not migrated to the repository-free format");
    }
    loaded = app::Settings{};
    require(app::load_settings(path, loaded, error), "Migrated settings load failed");
    require_settings_preserved();
    for (auto invalid : {"SCRCPY_PAD 1\n\"path\"\n\"shortcut\"\n\"unfinished repository", "SCRCPY_PAD 3\n\"path\"\n\"shortcut\"\n0 0 1\n\"Default\" \"--no-audio\"\n"}) {
      std::ofstream(path, std::ios::trunc) << invalid;
      error.clear();
      require(!app::load_settings(path, loaded, error) && !error.empty(), "Invalid settings format accepted");
      require_settings_preserved();
    }
    std::ofstream(path, std::ios::trunc) << "broken";
    require(!app::load_settings(path, loaded, error) && loaded.templates.size() == 4, "Corrupt settings changed current state");
    std::filesystem::remove(path); std::filesystem::remove(directory);
#ifdef _WIN32
    wchar_t exe_buffer[32768]{}; GetModuleFileNameW(nullptr, exe_buffer, 32768);
    auto exe = app::utf8(exe_buffer);
    const std::string newline = "\r\n";
#else
    auto exe = app::to_utf8(std::filesystem::read_symlink("/proc/self/exe"));
    const std::string newline = "\n";
#endif
    auto result = app::run_process(exe, {"--echo", "space arg", "\"quoted\"", "C:\\ending\\", "& echo literal"});
    require(result.code == 0 && result.text == "space arg" + newline + "\"quoted\"" + newline + "C:\\ending\\" + newline + "& echo literal" + newline, "Process argument round trip failed");
    result = app::run_process(exe, {"--wait"}, {}, 100);
    require(result.timed_out && result.code != 0, "Process timeout failed");
    auto cancel = std::make_shared<app::Cancel>();
    std::thread stopper([cancel] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); cancel->requested = true; });
    result = app::run_process(exe, {"--wait"}, {}, 0, cancel);
    stopper.join(); require(!result.timed_out && result.code != 0, "Process cancellation failed");
    result = app::run_process(app::to_utf8(std::filesystem::temp_directory_path() / "nonexistent-dir" / "scrcpy"), {});
    require(result.code != 0 && result.text.find("Could not start") != std::string::npos, "Launch failure was not reported");
    std::cout << "All core checks passed.\n";
    return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
