#include "scrcpy/Scrcpy.h"
#include "device/DeviceFiles.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <set>
#include <fstream>
#include <future>
#include <thread>

int main(int argc, char **argv) {
  try {
    if (argc < 2) throw std::runtime_error("Missing fixture");
    auto dir = std::filesystem::current_path() / "backend-fixtures" / std::to_string(app::process_id());
    std::filesystem::create_directories(dir / "space & path");
    auto executable = dir / "space & path" / (std::string("scrcpy") + app::executable_suffix);
    std::filesystem::copy_file(argv[1], executable);
    std::filesystem::copy_file(argv[1], executable.parent_path() / (std::string("adb") + app::executable_suffix));
    std::mutex mutex; std::condition_variable cv;
    std::string log, failure; std::vector<app::Device> devices;
    std::set<std::string> active; bool listed = false; int finished = 0;
    Scrcpy backend([&](std::string value) { std::lock_guard<std::mutex> lock(mutex); log += value; cv.notify_all(); },
      [&](std::string serial, bool running) {
        std::lock_guard<std::mutex> lock(mutex);
        if (running) active.insert(serial); else { active.erase(serial); ++finished; }
        cv.notify_all();
      });
    auto require = [](bool okay, const char *message) { if (!okay) throw std::runtime_error(message); };
    require(backend.set_path(app::to_utf8(executable)), "Could not set fixture path");
    backend.list_devices([&](std::vector<app::Device> value, std::string error) {
      std::lock_guard<std::mutex> lock(mutex); devices = value; failure = error; listed = true; cv.notify_all();
    });
    {
      std::unique_lock<std::mutex> lock(mutex);
      require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return listed; }), "Device discovery timed out");
      require(failure.empty() && devices.size() == 3 && devices[0].name == "Pixel 7 Pro", "Real fixture discovery failed");
    }
    require(backend.start("USB123", "--record=\"C:\\My Videos\\record.mp4\""), "First mirror did not start");
    require(backend.start("192.168.1.15:5555", "--no-audio"), "Second mirror did not start");
    require(!backend.start("USB123", ""), "Duplicate mirror was accepted");
    {
      std::unique_lock<std::mutex> lock(mutex);
      require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return log.find("--record=C:\\My Videos\\record.mp4") != std::string::npos && log.find("--serial=192.168.1.15:5555") != std::string::npos; }), "Arguments did not reach mirror processes");
    }
    backend.stop("USB123");
    {
      std::unique_lock<std::mutex> lock(mutex);
      require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return finished == 1; }), "Targeted stop timed out");
      require(active.size() == 1 && active.count("192.168.1.15:5555"), "Targeted stop affected another session");
    }
    backend.stop();
    {
      std::unique_lock<std::mutex> lock(mutex);
      require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return finished == 2; }), "Stop-all timed out");
    }
    require(app::device_path("/sdcard//Download/../Pictures/.") == "/sdcard/Pictures", "Remote normalization failed");
    require(app::device_parent("/") == "/" && app::device_parent("/sdcard/Download") == "/sdcard", "Remote parent failed");
    const auto adb = backend.adb_path();
    auto listing = app::list_device_files(adb, "USB123", "/sdcard/space ' & 雪", {});
    require(listing.error.empty() && listing.files.size() == 3, "Listing / quoted Unicode path failed");
    require(listing.files[0].directory && listing.files[0].name == "Download", "Folders were not sorted first");
    require(listing.files[2].name == "quote ' & 雪\nfile.txt" && listing.files[2].size == 1234, "Filename or size was corrupted");
    require(app::list_device_files(adb, "USB123", "/empty", {}).files.empty(), "Empty listing failed");
    require(app::list_device_files(adb, "USB123", "/empty", {}).error.empty(), "Empty folder reported error");
    require(!app::list_device_files(adb, "USB123", "/denied", {}).error.empty(), "Permission failure was hidden");
    require(!app::list_device_files(adb, "USB123", "/broken", {}).error.empty(), "Truncated listing accepted");
    require(!app::list_device_files(adb, "OFFLINE", "/sdcard", {}).error.empty(), "Disconnected device accepted");
    require(!app::list_device_files(adb, "USB123", "relative/path", {}).error.empty(), "Relative remote path accepted");
    require(!app::list_device_files(adb, "USB123", std::string("/sdcard\0oops", 12), {}).error.empty(), "NUL path accepted");
    auto read = [](const std::filesystem::path &path) {
      std::ifstream input(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(input), {});
    };
    auto local = dir / app::from_utf8("download & 雪.txt");
    std::ofstream(local) << "existing";
    require(!app::download_device_file(adb, "USB123", "/fail.bin", local, {}).empty(), "Failed pull reported success");
    require(read(local) == "existing", "Failed pull overwrote local file");
    auto cancelled = std::make_shared<app::Cancel>();
    auto pull = std::async(std::launch::async, [&] { return app::download_device_file(adb, "USB123", "/slow.bin", local, cancelled); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    cancelled->requested = true;
    require(pull.wait_for(std::chrono::seconds(3)) == std::future_status::ready && !pull.get().empty(), "Pull cancellation failed");
    require(read(local) == "existing", "Cancelled pull overwrote local file");
    require(app::download_device_file(adb, "USB123", "/sdcard/quote ' & 雪\nfile.txt", local, {}).empty(), "Pull failed");
    require(read(local) == "fixture download", "Successful pull content mismatch");
    for (const auto &entry : std::filesystem::directory_iterator(dir))
      require(entry.path().extension() != ".part", "Download left a staging file behind");
    auto second = dir / "second ' file.txt";
    std::ofstream(second) << "second file";
    require(app::upload_device_files(adb, "192.168.1.15:5555", "/sdcard/space ' & 雪", {local, second}, {}).empty(), "Multiple file upload failed");
    auto uploaded = executable.parent_path() / "uploaded";
    require(read(uploaded / local.filename()) == "fixture download" && read(uploaded / second.filename()) == "second file", "Uploaded files did not match");
    require(read(uploaded / "destination.txt") == "192.168.1.15:5555\n/sdcard/space ' & 雪/", "Upload did not target selected device/folder");
    require(!app::upload_device_files(adb, "USB123", "/sdcard/Download", {dir}, {}).empty(), "Directory upload was accepted");
    require(!app::upload_device_files(adb, "USB123", "/sdcard/Download", {dir / "missing"}, {}).empty(), "Missing file upload was accepted");
    auto failing = dir / "fail.txt"; std::ofstream(failing) << "fail";
    require(!app::upload_device_files(adb, "USB123", "/sdcard/Download", {failing}, {}).empty(), "Push failure was hidden");
    require(!app::upload_device_files(adb, "USB123", "/sdcard/Download", {local}, cancelled).empty(), "Cancelled upload was accepted");
    std::cout << "Device discovery, mirror sessions, folder browsing, downloads, uploads and cancellation passed.\n";
    return 0;
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
