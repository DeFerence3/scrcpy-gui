#pragma once
#include "main.h"
#include "device/DeviceFiles.h"
#include "platform/Desktop.h"

class FileExplorer : public std::enable_shared_from_this<FileExplorer> {
public:
  static std::shared_ptr<FileExplorer> create(slint::ComponentHandle<MainWindow> ui);
  ~FileExplorer();
  // Enables native file drops on `window` (an HWND on Windows; on Linux the
  // Slint window's Wayland surface is used and `window` is ignored).
  void attach(void *window);
  void detach();
  void open(void *window, const std::string &adb, const std::string &serial, const std::string &name);
  bool busy() const { return ui_->get_file_busy(); }
  void shutdown();
  void set_installing(bool value) { installing_ = value; }
private:
  explicit FileExplorer(slint::ComponentHandle<MainWindow> ui) : ui_(std::move(ui)) {}
  slint::ComponentHandle<MainWindow> ui_;
  std::string adb_;
  std::string serial_, folder_ = "/sdcard";
  std::vector<app::DeviceFile> files_;
  std::shared_ptr<app::Cancel> cancel_;
  std::vector<std::filesystem::path> pending_upload_;
  void *window_ = nullptr;
#ifndef _WIN32
  std::unique_ptr<desktop::FileDropTarget> drop_target_;
#endif
  bool registered_ = false, closed_ = false, installing_ = false;
  static void post(std::weak_ptr<FileExplorer> weak, std::function<void(FileExplorer &)> callback);
  void navigate(const std::string &path, const std::string &message = {}, bool failed = false);
  void upload(std::vector<std::filesystem::path> paths, bool confirmed = false);
  void pick_upload();
  void download(int index);
  void start_download(const std::string &remote, const std::string &label, const std::filesystem::path &local);
  void finish(const std::string &error, const std::string &success, bool refresh);
  void set_status(const std::string &message, bool failed = false);
  bool accepts_drop() const;
  void dropped(std::vector<std::filesystem::path> paths);
};
