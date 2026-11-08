# Building on Linux

This guide covers building, testing, installing and packaging scrcpy-gui on Linux from source. If you just want to use the app, a release tarball is easier (see the [README](../README.md#download)). For Windows, see [build-windows.md](build-windows.md).

- [Requirements](#requirements)
- [Install dependencies](#install-dependencies)
- [Build and run](#build-and-run)
- [Run the tests](#run-the-tests)
- [Install](#install)
- [Build the release tarball](#build-the-release-tarball)
- [Build options](#build-options)
- [Using a prebuilt Slint SDK](#using-a-prebuilt-slint-sdk)
- [Desktop setup after installing](#desktop-setup-after-installing)
- [Troubleshooting](#troubleshooting)

---

## Requirements

**To build:**

- CMake 3.21 or newer, and Ninja
- A C++17 compiler (GCC 9+ or Clang 10+)
- Rust (stable), when CMake builds Slint from source (the default)
- `pkg-config`, plus development headers for **libsystemd** (sd-bus), **wayland-client**, **xkbcommon** and **fontconfig**

**To run:**

- A Wayland or X11 session with a **session D-Bus**
- A tray that supports **StatusNotifierItem**: Waybar, Quickshell/Omarchy, KDE Plasma, or GNOME with the AppIndicator extension
- An **xdg-desktop-portal** backend with a file chooser, such as `xdg-desktop-portal-gtk`, `-kde` or `-gnome`
- **scrcpy** and **adb**
- For self-updates only: `curl`, `tar` and `sha256sum`

The app is tested on Hyprland. Release binaries are built on Ubuntu 22.04, so they need glibc 2.35 or newer.

## Install dependencies

**Arch Linux / Omarchy**

```sh
sudo pacman -S --needed base-devel cmake ninja rust pkgconf systemd-libs wayland libxkbcommon fontconfig \
    scrcpy android-tools xdg-desktop-portal-gtk
```

**Debian / Ubuntu (22.04+)**

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
    libsystemd-dev libwayland-dev libxkbcommon-dev libfontconfig1-dev \
    scrcpy adb xdg-desktop-portal-gtk
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh    # distro Rust is often too old for Slint
```

**Fedora**

```sh
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \
    systemd-devel wayland-devel libxkbcommon-devel fontconfig-devel \
    rust cargo scrcpy android-tools xdg-desktop-portal-gtk
```

Package names and versions vary between distributions. If a distribution's scrcpy is old, use the [official scrcpy release](https://github.com/Genymobile/scrcpy/blob/master/doc/linux.md).

## Build and run

```sh
git clone https://github.com/DeFerence3/scrcpy-gui-slint.git
cd scrcpy-gui-slint

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/app/scrcpy-ui
```

- The first configure downloads and builds Slint, which takes several minutes. Later builds are incremental.
- On Linux, `CMakeLists.txt` already sets the Slint options: the Qt backend and the Skia/FemtoVG renderers are off, and the software renderer is on. Slint uses winit for Wayland/X11, and the app avoids loading GPU drivers (about 90 MB of memory saved for this small panel).
- Only one copy runs at a time. If one is already running, `./build/app/scrcpy-ui` just shows it. Quit it from the tray menu (**Quit**) before testing a new build.

## Run the tests

```sh
ctest --test-dir build --output-on-failure
```

On Linux this runs `scrcpy-core` (settings, parsing, argument splitting, JSON, updater versions and release parsing), `scrcpy-backend` (scrcpy/ADB handling against a fake tool executable) and `scrcpy-ui-preview`. The preview test renders every page to `build/ui-previews/*.ppm` without a display and tests click targets. Open the images to check UI changes. The Windows-only tests (file drop, startup, PowerShell scripts, installer) aren't built.

## Install

Install only the app's files (the `gui` component), not Slint's headers and tools:

```sh
cmake --install build --component gui --prefix ~/.local --strip
```

This installs:

| Path | File |
|---|---|
| `~/.local/bin/scrcpy-ui` | The app (finds its library through `RPATH=$ORIGIN/../lib`) |
| `~/.local/lib/libslint_cpp.so` | The Slint runtime library |
| `~/.local/share/applications/scrcpy-gui.desktop` | Launcher entry |
| `~/.local/share/icons/hicolor/scalable/apps/scrcpy-gui.svg` | Icon |
| `~/.local/share/doc/scrcpy-gui/` | README and license |

An install in a folder you can write to, such as `~/.local`, can update itself from **Settings → Check updates**. A system-wide install (`sudo cmake --install build --component gui --prefix /usr/local`) works too, but it only *reports* updates; rebuild it to update.

To uninstall, delete the files listed above.

## Build the release tarball

```sh
cpack --config build/CPackConfig.cmake -B build/packages
```

This produces `build/packages/scrcpy-gui-linux-x64.tar.gz` (or `-arm64`). The archive has a flat `bin/ lib/ share/` layout with no top-level folder, so users install it with:

```sh
tar -xzf scrcpy-gui-linux-x64.tar.gz -C ~/.local
```

The self-updater expects this layout and this file name.

## Build options

| Option | Default | Meaning |
|---|---|---|
| `SCRCPY_VERSION` | `0.0.2` | App and package version, shown in the footer and used by the updater. CI sets it from the release tag. |
| `CMAKE_BUILD_TYPE` | — | `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel` |
| `BUILD_TESTING` | `ON` | Build the CTest targets |
| `Slint_DIR` | — | Path to a prebuilt Slint SDK's `lib/cmake/Slint` |

To test update detection, build with an older version (`-DSCRCPY_VERSION=0.0.1`) and use **Settings → Check updates**.

## Using a prebuilt Slint SDK

CMake uses an installed Slint package when it finds one, and downloads and builds Slint otherwise. To build Slint once and reuse it (this is what CI does):

```sh
git clone --depth 1 --branch v1.18.1 https://github.com/slint-ui/slint.git build/deps/slint
cmake -S build/deps/slint/api/cpp -B build/slint-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PWD/build/slint-sdk" \
    -DSLINT_BUILD_TESTING=OFF \
    -DSLINT_FEATURE_BACKEND_QT=OFF \
    -DSLINT_FEATURE_RENDERER_SKIA=OFF \
    -DSLINT_FEATURE_RENDERER_FEMTOVG=OFF \
    -DSLINT_FEATURE_RENDERER_SOFTWARE=ON
cmake --build build/slint-build
cmake --install build/slint-build

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSlint_DIR="$PWD/build/slint-sdk/lib/cmake/Slint"
```

The Slint library from the SDK is installed and packaged with the app as usual.

## Desktop setup after installing

- **Autostart:** add `scrcpy-ui --startup` to your compositor's or desktop's autostart.
- **Shortcut:** bind a key to `scrcpy-ui --quick-mirror`. See [features.md](features.md#linux) for Hyprland, Sway, i3, GNOME and KDE examples.
- **Window placement:** on Hyprland the app sets up its own float/pin rule. On other compositors, add a float rule for app id `scrcpy-gui`.

## Troubleshooting

| Problem | Fix |
|---|---|
| `Package 'libsystemd' not found` / `wayland-client` not found | Install the `-dev`/`-devel` packages listed above, plus `pkg-config`. |
| Slint fails to build with a Rust error | Use a current stable Rust from rustup (`rustup update stable`). |
| Linking fails on `xkbcommon` or `fontconfig` | Install `libxkbcommon-dev` and `libfontconfig1-dev` (or your distribution's equivalent). |
| No tray icon | Your bar or desktop needs StatusNotifierItem support (Waybar `tray` module, KDE, or GNOME with the AppIndicator extension). Without a tray, the panel stays visible. |
| Browse / Save As does nothing | Install and start an `xdg-desktop-portal` backend with a file chooser (for example `xdg-desktop-portal-gtk`). |
| Panel opens tiled or in the wrong place | On compositors other than Hyprland, add a float rule for app id `scrcpy-gui`. |
| `error while loading shared libraries: libslint_cpp.so` | Keep `bin/` and `lib/` side by side as installed; the binary looks in `../lib`. |
| Drag-and-drop upload doesn't work | Drops are supported in Wayland sessions; under X11, use the upload icon. |
| Update says it can't install | The install folder isn't writable (for example under `/usr`), or you're running from `build/`. Reinstall into `~/.local` or update manually. |
