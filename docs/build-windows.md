# Building on Windows

This guide covers building, testing, running and packaging scrcpy-gui on Windows from source. If you just want to use the app, download a release instead (see the [README](../README.md#download)). For Linux, see [build-linux.md](build-linux.md).

- [Requirements](#requirements)
- [Get the source](#get-the-source)
- [Quick start: `run.ps1`](#quick-start-runps1)
- [Manual build](#manual-build)
- [Run the tests](#run-the-tests)
- [Build packages](#build-packages)
- [Build options](#build-options)
- [Using a prebuilt Slint SDK](#using-a-prebuilt-slint-sdk)
- [Troubleshooting](#troubleshooting)

---

## Requirements

| Tool | Notes |
|---|---|
| Windows 10 or 11 (x64) | ARM64 builds also package, but releases are x64 |
| [Git](https://git-scm.com/download/win) | |
| [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) or the Build Tools | Workload **Desktop development with C++**, including the **Windows SDK** |
| [CMake](https://cmake.org/download/) 3.21 or newer | Included with Visual Studio's C++ workload |
| [Ninja](https://github.com/ninja-build/ninja/releases) | Included with Visual Studio's C++ workload |
| [Rust](https://rustup.rs) (stable, MSVC toolchain) | Only when CMake builds Slint from source (the default) |
| [NSIS](https://nsis.sourceforge.io/Download) 3.x | Optional; needed only for the installer |
| [scrcpy](https://github.com/Genymobile/scrcpy/releases) | To actually run and mirror; not needed to build |

Using `winget`:

```powershell
winget install Git.Git Kitware.CMake Ninja-build.Ninja Rustlang.Rustup NSIS.NSIS Genymobile.scrcpy
winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive"
```

Run every command below in **Developer PowerShell for VS 2022** (Start menu), so `cl.exe` and the Windows SDK are on `PATH`.

## Get the source

```powershell
git clone https://github.com/DeFerence3/scrcpy-gui-slint.git
cd scrcpy-gui-slint
```

## Quick start: `run.ps1`

For day-to-day development:

```powershell
./run.ps1                          # Debug
./run.ps1 -Configuration Release   # Release (also RelWithDebInfo, MinSizeRel)
```

The script:

1. Configures `build/` (again each time, so an interrupted configure can't leave it broken) with the recommended Slint options.
2. Stops a copy of the app that is already running from `build/app`.
3. Builds the `scrcpy-ui` target and its dependencies only (not the tests), and stops on the first error.
4. Starts the new build.

The first configure downloads and builds Slint, which takes several minutes. Later builds are incremental.

## Manual build

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DSLINT_FEATURE_BACKEND_QT=OFF `
    -DSLINT_FEATURE_RENDERER_SKIA=OFF `
    -DSLINT_FEATURE_RENDERER_SOFTWARE=ON
cmake --build build --parallel
.\build\app\scrcpy-ui.exe
```

- The Slint options keep the build small. They turn off the Qt backend and the Skia renderer and use the software renderer, which is also needed for the UI preview test.
- The executable goes to `build\app\`. The build copies the Slint DLL and `update.ps1` next to it. **Keep `scrcpy-ui.exe`, its DLLs and `update.ps1` together** when you copy the build somewhere else. The updater depends on `update.ps1`.
- Quit any running copy (tray → Settings → **Quit app**) before you test or start a new build. Only one instance can run at a time, and a new launch just brings the old one to the front.

### Visual Studio generator

When the build folder uses a Visual Studio generator instead of Ninja (for example `-G "Visual Studio 17 2022"`, or opening the folder in Visual Studio), the configuration is chosen at build time:

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\app\Release\scrcpy-ui.exe
```

### Opening in an IDE

- **Visual Studio:** *File → Open → Folder* and choose the repository. Visual Studio reads `CMakeLists.txt` directly. Add the Slint options above to `CMakeSettings.json` or a `CMakeUserPresets.json`.
- **VS Code:** install the *CMake Tools* and *Slint* extensions and start VS Code from Developer PowerShell (`code .`) so it inherits the compiler environment.

If you configure from an ordinary terminal that the Developer environment was never loaded into, the build uses `cmake/msvc-env.cmd` to restore it for Ninja. Configuring from Developer PowerShell the first time is still the most reliable.

## Run the tests

```powershell
ctest --test-dir build --output-on-failure
```

On Windows this runs:

- `scrcpy-core`: settings, parsing, argument splitting, JSON and versions.
- `scrcpy-backend`: scrcpy/ADB handling against a fake tool executable.
- `scrcpy-file-drop`: the OLE drop target.
- `scrcpy-startup`: starts the real app, single instance, `--startup` and Quick Mirror. Skipped (exit code 77) when it can't run.
- `scrcpy-run-script` and `scrcpy-updater`: PowerShell tests for `run.ps1` and `update.ps1`.
- `scrcpy-installer`: builds and runs the NSIS installer silently. Only when `makensis` is found.
- `scrcpy-ui-preview`: renders every page to `build\ui-previews\*.ppm` and tests click targets.

Quit the app before running the tests; the startup and installer tests start their own copies. To run one test: `ctest --test-dir build -R scrcpy-backend --output-on-failure`.

## Build packages

Configure and build in **Release** first, then:

```powershell
# Portable ZIP (also what the self-updater downloads)
cpack --config build/CPackConfig.cmake -C Release -G ZIP -B build/packages

# Installer (requires NSIS)
cpack --config build/CPackConfig.cmake -C Release -G NSIS -B build/packages
```

Output in `build\packages\`:

| File | Description |
|---|---|
| `scrcpy-gui-windows-x64.zip` | Portable. Extract anywhere and run `scrcpy-ui.exe`. Doesn't register for startup. |
| `scrcpy-gui-windows-x64.exe` | Per-user installer (no admin needed; writes to HKCU). Adds Start-menu entries and startup at sign-in, and starts the app in the tray. |

Both contain `scrcpy-ui.exe`, the Slint and Visual C++ runtime DLLs, `update.ps1`, `README.md` and `LICENSE.txt`.

## Build options

| Option | Default | Meaning |
|---|---|---|
| `SCRCPY_VERSION` | `0.0.2` | App and package version, shown in the footer and used by the updater. CI sets it from the release tag. |
| `CMAKE_BUILD_TYPE` | — | `Debug`, `Release`, `RelWithDebInfo` or `MinSizeRel` (single-config generators) |
| `BUILD_TESTING` | `ON` | Build the CTest targets |
| `SLINT_FEATURE_BACKEND_QT` | Slint default | Set `OFF` |
| `SLINT_FEATURE_RENDERER_SKIA` | Slint default | Set `OFF` |
| `SLINT_FEATURE_RENDERER_SOFTWARE` | Slint default | Set `ON` (needed for `scrcpy-ui-preview`) |

Example: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DSCRCPY_VERSION=0.1.1" ...`

To test update detection, build with an older version such as `"-DSCRCPY_VERSION=0.0.1"` and use **Settings → Check updates**.

## Using a prebuilt Slint SDK

CMake first looks for an installed Slint package and only downloads and builds Slint when none is found. Building Slint once and reusing it saves time (and makes Rust unnecessary for the app build). This is what CI does:

```powershell
git clone --depth 1 --branch v1.18.1 https://github.com/slint-ui/slint.git build/deps/slint
cmake -S build/deps/slint/api/cpp -B build/slint-build -G "Visual Studio 17 2022" -A x64 `
    "-DCMAKE_INSTALL_PREFIX=$PWD/build/slint-sdk" `
    -DSLINT_BUILD_TESTING=OFF `
    -DSLINT_FEATURE_BACKEND_QT=OFF `
    -DSLINT_FEATURE_RENDERER_SKIA=OFF `
    -DSLINT_FEATURE_RENDERER_SOFTWARE=ON
cmake --build build/slint-build --config Release
cmake --install build/slint-build --config Release

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DSlint_DIR=$PWD/build/slint-sdk/lib/cmake/Slint" `
    -DSLINT_FEATURE_RENDERER_SOFTWARE=ON
```

Use a Slint 1.x version. Without an SDK, the project tracks the `release/1` branch.

## Troubleshooting

| Problem | Fix |
|---|---|
| `cl.exe` not found, or `windows.h` / `kernel32.lib` missing | Use **Developer PowerShell for VS 2022**, and check that the C++ workload and the Windows SDK are installed. |
| `C1090` / PDB errors in Debug | Already handled: the build switches `/Zi` to `/Z7`. Delete `build/` if an old cache still has `/Zi`. |
| `cargo` / `rustc` not found while configuring | Install Rust with rustup (MSVC toolchain) and open a new terminal, or use a [prebuilt Slint SDK](#using-a-prebuilt-slint-sdk). |
| App starts but nothing happens | A copy is already running in the tray. Quit it first. |
| `slint_cpp.dll` not found | Run the exe from `build\app\`, or copy every DLL next to it. |
| Installer test skipped | NSIS isn't installed or `makensis` isn't in `Program Files\NSIS`. |
| Configure fails after an interrupted run | Delete `build\CMakeCache.txt` (or all of `build\`) and configure again, or use `run.ps1`. |
