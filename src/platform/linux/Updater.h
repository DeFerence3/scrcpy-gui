#pragma once
#include "platform/Platform.h"
#include <cstdint>
#include <filesystem>
#include <string>

// Linux self-update. Mirrors scripts/update.ps1: GitHub's releases/latest is
// checked with curl, the portable tarball is verified against GitHub's SHA-256
// digest, staged, and installed over the running copy (files are replaced by
// rename, which is safe while the old executable runs).
namespace update {
struct Asset {
  std::string url, sha256;
  uint64_t size = 0;
};
// "scrcpy-gui-linux-x64.tar.gz" (or arm64) for this build.
std::string asset_name();
// Compares dotted numeric versions; returns <0, 0 or >0. Invalid input throws.
int compare_versions(const std::string &a, const std::string &b);
bool valid_version(const std::string &version);
// Reads releases/latest metadata. Returns an error message, empty on success.
std::string parse_release(const std::string &json, const std::string &repository, const std::string &asset,
                          std::string &version, Asset &result);
// Whether `name` (a path inside the tarball) is a file the app installs.
bool allowed_entry(const std::string &name);

std::filesystem::path state_directory();
// The installation prefix (the folder holding bin/scrcpy-ui), or empty when
// this copy cannot update itself (build tree, read-only or /usr installs).
std::filesystem::path install_prefix(std::string &reason);

// Runs the check (stage = false) or download (stage = true). The output uses
// update.ps1's markers: __NONE__|message, __AVAILABLE__|version,
// __MANUAL__|version (no self-update for this install) and __READY__|stage.
app::ProcessResult run(bool stage, const std::string &repository, const std::string &current,
                       const std::string &expected);
// Checks that a release tarball holds only plain files the app installs (no
// links, traversal or unexpected names), then extracts it into `directory`.
std::string extract(const std::filesystem::path &archive, const std::filesystem::path &directory);
// Installs a staged release into `prefix`, rolling back on failure.
std::string apply(const std::filesystem::path &stage, const std::filesystem::path &prefix);
}
