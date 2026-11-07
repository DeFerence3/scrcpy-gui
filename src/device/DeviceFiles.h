#pragma once
#include "platform/Platform.h"
#include <cstdint>

namespace app {
struct DeviceFile {
  std::string name;
  bool directory = false;
  uint64_t size = 0;
};
struct DirectoryResult {
  std::vector<DeviceFile> files;
  std::string error;
};
std::string device_path(const std::string &path);
std::string device_child(const std::string &folder, const std::string &name);
std::string device_parent(const std::string &path);
DirectoryResult list_device_files(const std::string &adb, const std::string &serial,
                                 const std::string &path, std::shared_ptr<Cancel> cancel);
// Empty error means success. Downloads are staged beside the destination, so a
// failed or cancelled pull never overwrites an existing local file.
std::string download_device_file(const std::string &adb, const std::string &serial,
                                const std::string &remote, const std::filesystem::path &local,
                                std::shared_ptr<Cancel> cancel);
std::string upload_device_files(const std::string &adb, const std::string &serial,
                               const std::string &folder,
                               const std::vector<std::filesystem::path> &files,
                               std::shared_ptr<Cancel> cancel, Output progress = {});
}
