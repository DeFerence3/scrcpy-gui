# scrcpy-gui

A compact tray app for [scrcpy](https://github.com/Genymobile/scrcpy), built with C++ and Slint. Mirror and control Android devices, manage scrcpy presets, and start sessions with a keyboard shortcut.

Supports **Windows** and **Linux** (tested on Hyprland). Install scrcpy and ADB separately; they are not bundled with this app. See [Linux](#linux) for what differs there.

## Use

1. Install scrcpy with its included `adb.exe`.
2. Install the GUI setup package, or extract the portable ZIP and run `scrcpy-ui.exe`. The installer starts the app in the tray and enables startup at sign-in.
3. Enable USB debugging on your Android device, connect it, and accept the authorization prompt.
4. Open the tray icon. Click the **Settings gear** beside the pin icon and browse to `scrcpy.exe` if it was not detected automatically.
5. In **Devices**, click the template name on a device card to start mirroring, or click its dropdown arrow and choose another template to start that configuration. The control changes to Stop while mirroring.

Use **Make Default** in **Manage templates** to choose the preset for device buttons and **Quick Mirror**. The default shortcut is **Ctrl + Alt + S**; with multiple ready devices, the app asks you to choose one.

Closing the window hides it to the tray and resets it to **Devices** for the next opening. The **pin icon** keeps it open when focus changes and highlights while pinned; **Quit app** exits. Portable copies do not enable startup at sign-in.

Click the **folder icon** to the left of Mirror to explore a device's files inside the tray panel. The header and version footer stay visible; the **back arrow** returns to devices. Browsing starts at `/sdcard`. Use the **up**, **home**, and **refresh** icons to navigate, or enter an absolute path and press Enter. The arrow beside a folder opens it; the **download icon** beside a file opens Save As. Drop local files into the panel, or use the **upload icon**, to copy them into the open folder. The Files page stays visible when focus changes so you can drag from Windows Explorer. Hover over an icon for its label. Existing files prompt before replacement. Use the **cancel icon** to stop a transfer; an interrupted upload may leave a partial file on the device. Android's normal ADB storage permissions apply.

## Features

- Discover USB and wireless devices connected through ADB, with connection and authorization status.
- Mirror multiple devices and stop sessions individually or together.
- Browse device storage, download files, and upload files by drag and drop.
- Create, edit, and save scrcpy argument templates, including resolution, frame rate, and audio preferences.
- Customize the global Quick Mirror shortcut.
- Keep the app in the tray, with optional window pinning and automatic startup through the installer.
- View logs, detect scrcpy on PATH, and restart ADB from Settings.
- Check for GUI updates from `DeFerence3/scrcpy-gui-slint`, or enable automatic updates. These updates do not replace scrcpy.

Preferences are stored in `%APPDATA%\scrcpy-tray\settings.dat`.

## Linux

The Linux build uses no GUI toolkit besides Slint: the tray icon is a StatusNotifierItem over D-Bus (shown by Waybar, Quickshell/Omarchy, KDE and similar bars), and file dialogs use `xdg-desktop-portal`. It needs `libsystemd`, `libwayland-client`, a session D-Bus, and a portal backend with a file chooser such as `xdg-desktop-portal-gtk`.

- Left-click the tray icon to show or hide the panel; right-click for **Quick Mirror**, **Show / hide** and **Quit**.
- On Hyprland the app sets its own window rule at startup over Hyprland's IPC socket, so the panel floats in the top-right corner beside the tray, pinned across workspaces. It sets the rule again after `hyprctl reload`. Other Wayland compositors decide placement themselves; add a float rule for the app id (class) `scrcpy-gui` there.
- Drag files from your file manager onto the **Files** page to upload them to the open device folder (Wayland sessions).
- The panel stays open when it loses focus; close it with **×** or the tray icon.
- Preferences are stored in `$XDG_CONFIG_HOME/scrcpy-tray/settings.dat` (usually `~/.config/scrcpy-tray/`).

### Keyboard shortcut

Wayland does not let apps grab global keys, and the app never changes your compositor's key bindings. Instead, bind a key to this command:

```sh
scrcpy-ui --quick-mirror
```

When the app is running, this asks it to run **Quick Mirror** and then exits immediately: with exactly one ready device, mirroring starts right away; otherwise the panel opens so you can choose a device. When the app is not running, the command starts it and runs Quick Mirror. Plain `scrcpy-ui` shows the panel and starts Quick Mirror; `scrcpy-ui --startup` starts in the tray without showing anything (for autostart).

Use the full path (for example `~/.local/bin/scrcpy-ui`) if the install folder is not on the compositor's `PATH`. Examples:

- **Hyprland, Lua config** (Omarchy, in `~/.config/hypr/bindings.lua` and `autostart.lua`):

  ```lua
  o.bind("SUPER + CTRL + M", "Quick Mirror", "scrcpy-ui --quick-mirror")
  o.launch_on_start("scrcpy-ui --startup")
  ```

  Without Omarchy's helpers: `hl.bind("SUPER + CTRL + M", hl.dsp.exec_cmd("scrcpy-ui --quick-mirror"))`.
- **Hyprland, `hyprland.conf`**:

  ```ini
  bind = SUPER CTRL, M, exec, scrcpy-ui --quick-mirror
  exec-once = scrcpy-ui --startup
  ```

- **Sway / i3** (`~/.config/sway/config`):

  ```
  bindsym $mod+Ctrl+m exec scrcpy-ui --quick-mirror
  exec scrcpy-ui --startup
  ```

- **GNOME**: Settings → Keyboard → View and Customize Shortcuts → Custom Shortcuts, with the command above.
- **KDE Plasma**: System Settings → Keyboard → Shortcuts → Add New → Command or Script.

Pick a free combination; check what is taken with `hyprctl binds` (Omarchy already uses Super + Alt + S).

### Updates

Release tarballs (`scrcpy-gui-linux-x64.tar.gz`) update themselves like the Windows app: **Settings → Check updates** downloads the newest GitHub release, verifies its SHA-256 digest, and replaces the files in place before restarting. This needs `curl`, `tar` and `sha256sum`, and an installation you can write to without `sudo`, such as `~/.local`. Copies installed under `/usr`, into a folder you cannot write to, or run from a build folder only report that an update is available; update those the way you installed them.

To install a release tarball into `~/.local`:

```sh
tar -xzf scrcpy-gui-linux-x64.tar.gz -C ~/.local
```

### Build from source

Build requirements: CMake 3.21+, Ninja, a C++17 compiler, Rust (to build Slint), `pkg-config`, and the `libsystemd` and `wayland` headers. On Arch: `sudo pacman -S cmake ninja rust scrcpy`.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/app/scrcpy-ui
cmake --install build --component gui --prefix ~/.local --strip   # binary, Slint library, .desktop entry, icon
cpack --config build/CPackConfig.cmake -B build/packages            # release tarball
```

## Build locally

These steps are for Windows; see [Linux](#linux) for Linux builds.

Requirements: Windows, Git, CMake 3.21+, Ninja, and Visual Studio C++ build tools with the Windows SDK. Install stable Rust if building Slint from source. CMake uses an installed Slint package when available; otherwise it downloads and builds Slint.

From the repository directory in **Visual Studio Developer PowerShell**:

For a Debug build that stops the previous local instance and launches the app, run `./run.ps1`. The script configures the build automatically, builds the app target and its dependencies, and stops on errors. Use `./run.ps1 -Configuration Release` for Release. The commands below also build the test targets.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DSLINT_FEATURE_BACKEND_QT=OFF `
    -DSLINT_FEATURE_RENDERER_SKIA=OFF `
    -DSLINT_FEATURE_RENDERER_SOFTWARE=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
.\build\app\scrcpy-ui.exe
```

Quit any running copy before testing or launching the new build. Keep the executable, runtime DLLs, and `update.ps1` together.

If your build directory uses a Visual Studio generator, build with `--config Release`, test with `-C Release`, and launch `.\build\app\Release\scrcpy-ui.exe`.

To create distribution packages:

```powershell
# Portable ZIP
cpack --config build/CPackConfig.cmake -C Release -G ZIP -B build/packages

# Installer (requires NSIS)
cpack --config build/CPackConfig.cmake -C Release -G NSIS -B build/packages
```

Packages are written to `build/packages`. To set the app and package version, add `"-DSCRCPY_VERSION=0.1.1"` when configuring CMake.

## Publish a release

Push a new version tag on the commit to release:

```powershell
git tag v0.0.3
git push origin v0.0.3
```

Use `vMAJOR.MINOR.PATCH` (or `vMAJOR.MINOR.PATCH.REVISION`) and a version higher than the previous release. The distribution workflow builds and tests Windows x64 and Linux x64, packages the portable ZIP, the installer and the Linux tarball, then publishes a GitHub release with all three packages and `SHA256SUMS.txt`. The app version comes from the tag. Publishing uses the repository's `PAT` Actions secret, which must have write access to repository contents (or the `repo` scope for a classic PAT).

Pushes to `master` also build and test the app to warm the compiled Slint SDK cache. Let that run finish before pushing a release tag: GitHub lets tags restore default-branch caches, but isolates caches created by different tags. A cache hit skips the Slint checkout, Rust setup, and Slint build; the app and tests always build from the checked-out source. The cache key includes the platform, pinned Slint/Rust versions, and workflow hash, so changes to SDK build options invalidate it. Cache misses (including eviction) rebuild the SDK automatically. Use **Actions → Build and release distribution → Run workflow** on `master` to warm it manually; branch and manual runs do not publish releases.

The app checks the latest published, non-prerelease GitHub release and offers an update only when its version is newer. Keep the assets named `scrcpy-gui-windows-x64.zip` and `scrcpy-gui-linux-x64.tar.gz`; the updaters verify their GitHub SHA-256 digests before installation. Workflow artifacts alone are not visible to the updater.

To test update detection locally, configure a build with a lower version, such as `"-DSCRCPY_VERSION=0.0.1"`, rebuild, and use **Settings → Check updates**. Builds made before the repository address was corrected must first be rebuilt or manually replaced with a current release.
