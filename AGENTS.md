# Repository Guidelines

## Project Structure & Module Organization

This tray app wraps scrcpy using C++17 and Slint. It targets Windows and Linux (Wayland/X11).

Sources in `src/` are grouped by area:

- `src/main.cpp` connects the UI, tray, hotkeys, and update workflow (`scripts/update.ps1` on Windows, `platform/linux/Updater_posix.cpp` on Linux).
- `src/core/` holds portable logic with no OS or UI dependencies: `AppCore.*` (settings, device parsing) and `Json.*` (a small reader for GitHub and Hyprland replies).
- `src/scrcpy/` manages scrcpy and ADB: discovery, versions and mirror sessions (`Scrcpy.*`).
- `src/device/` covers device browsing and transfers: `DeviceFiles.*` (adb file operations), `FileExplorer.*` (the Files page) and `DeviceModel.h` (the devices list model).
- `src/platform/` declares the portable OS interfaces: `Platform.h` (process/path helpers, UTF-8 strings everywhere; shared code in `Platform.cpp`) and `Desktop.h` (tray, hotkey, single instance, window placement, file dialogs, file drops).
- `src/platform/windows/` implements them with Win32/COM (`Platform_win.cpp`, `Desktop_win.cpp`, the OLE `FileDropTarget.*`).
- `src/platform/linux/` implements them for Linux (`Platform_posix.cpp`; `Desktop_linux.cpp` with the sd-bus StatusNotifierItem + dbusmenu, xdg-desktop-portal, Unix socket, Hyprland window rule and Wayland file drops) and holds the Linux self-updater (`Updater.*`). On Linux the app never edits compositor key bindings; users bind `scrcpy-ui --quick-mirror`.

Source layout rules:

- Put every new file in the subdirectory for its area; only `main.cpp` lives directly in `src/`. Add a new subdirectory when a group of related files fits none of the existing ones, and describe it here.
- OS-specific code goes in `src/platform/windows/` or `src/platform/linux/`, with the OS in the file name (`_win`, `_linux`, `_posix`). Portable interfaces go in `src/platform/`. Keep OS-specific code in these files rather than adding `#ifdef`s elsewhere.
- Write includes relative to `src/` (for example `#include "platform/Platform.h"`), never bare file names or `../` paths, except `tests/ui_preview.cpp`, which does not link `scrcpy-core`.
- When moving or adding sources, update the source lists in `CMakeLists.txt` and this section.

Other directories:

- `ui/main.slint` defines the interface, reusable controls, and theme. No separate asset directory is present.
- `tests/` contains core/backend checks, a mock tool executable, updater fixtures, and headless UI previews.
- `README.md` is for users. `docs/` holds the detailed guides: `features.md`, `architecture.md`, `build-windows.md` and `build-linux.md`, plus screenshots in `docs/images/` (rendered by `scrcpy-ui-preview`). Update the matching doc when behavior, layout or build steps change.
- `scripts/update.ps1` implements Windows updates; `packaging/linux/` holds the `.desktop` entry and icon; `cmake/` contains the MSVC environment helper. Root `CMakeLists.txt` defines targets, tests, and packaging.
- Keep generated code, binaries, and fetched dependencies inside build directories; do not commit them.

## Build, Test, and Development Commands

On Linux, use CMake 3.21+, Ninja, a C++17 compiler, Rust, pkg-config, and the libsystemd and wayland-client headers; the commands are the same with `./build/app/scrcpy-ui`. On Windows, use Visual Studio Developer PowerShell with CMake 3.21+, Ninja, Visual Studio C++ tools, and the Windows SDK. Rust is required when CMake builds Slint from source instead of using an installed package.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
.\build\app\scrcpy-ui.exe
cpack --config build/CPackConfig.cmake -C Release -B build/packages
```

These configure, build, test, launch, and package the app. For Visual Studio generators, build with `--config Release`, test with `-C Release`, and launch `build\app\Release\scrcpy-ui.exe`. Keep runtime DLLs and `update.ps1` beside the executable.

## Coding Style & Naming Conventions

Match surrounding formatting: C++ generally uses two-space indentation; Slint and PowerShell use four spaces. Use `PascalCase` types/components, `snake_case` C++ functions and variables, and kebab-case Slint properties. Headers use `#pragma once`. No repository formatter or linter configuration is present; avoid unrelated reformatting.

## Testing Guidelines

CTest runs standalone C++ assertion helpers and PowerShell fixtures, without a third-party test framework or enforced coverage threshold. Extend `tests/*_tests.cpp` or `tests/updater_tests.ps1` for behavior changes, including failure paths. Use `tool_fixture.cpp` for simulated processes/devices. With Slint's software renderer enabled, `scrcpy-ui-preview` produces images in `build/ui-previews`; inspect these for UI changes.

## Commit & Pull Request Guidelines

History contains only `initial commit`, so no established message convention exists. Use concise imperative subjects, such as `Fix device discovery cancellation`. Keep commits focused. PRs should explain the behavior change, link relevant issues, report validation commands/results, and include screenshots or previews for UI changes.
