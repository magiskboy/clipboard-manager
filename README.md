# clipboard-manager

Cross-platform clipboard manager written in C11. The UI uses [libui-ng](https://github.com/libui-ng/libui-ng), vendored as a Git submodule at `vendors/libui-ng`.

## Development

### Getting the source

```sh
git clone --recursive <repository-url>
cd clipboard-manager
```

If you already cloned without submodules:

```sh
git submodule update --init --recursive
```

### Dependencies and tools

**Toolchain:** a C11 compiler, **C++** (libui-ng is built with Meson as a C and C++ project), GNU Make, and Git.

**Linux (Fedora example)** — enough to build libui-ng against GTK 3:

```sh
sudo dnf install gcc gcc-c++ make git gtk3-devel
```

**Meson and Ninja** (used to build the library in `vendors/libui-ng`):

```sh
pip install --user meson ninja
export PATH="$HOME/.local/bin:$PATH"
```

You can install `meson` and `ninja` (or `ninja-build`) from your distribution instead.

**macOS:** install the Xcode Command Line Tools (`xcode-select --install`). The application uses Objective-C (`darwin.m`) and links against Cocoa.

**Windows:** use a suitable environment (e.g. MSYS2/MinGW or MSVC) and install Meson and Ninja for that environment.

### Clipboard behavior (Phase 1–2)

The main window lists recent **plain text** clipboard entries (newest first). **Double-click** a row to open the full-text detail window. **Right-click** a row (or **Control-click** on macOS) for a context menu with **Copy**, **Remove**, and **Clear all**. History size is capped in memory (default 100 items; configurable under **Edit → Settings**). The **Source** column shows a best-effort label for the application that owned the clipboard when the text was captured (see limitations below); if unknown, it shows an em dash.

Closing the main window **hides** the app to the **system tray** (when the desktop exposes a tray area). Use **Show window** from the tray menu to reopen, or **Quit** to exit completely. The menu bar includes **Settings** (preferences), **About**, and **Quit**.

Configuration is stored as `config.ini` under the OS-appropriate user config directory (for example `~/.config/clipboard-manager/` on Linux with XDG defaults). Settings include maximum history size and a **global shortcut** string saved for a future Phase 3 (system-wide registration is not active yet).

The vendored **libui-ng** fork adds `uiTableOnRowRightClicked()` so secondary-click events are available on all supported platforms.

### Known limitations

- **Plain text only** for clipboard capture and copy (rich text or images are out of scope for Phase 1).
- **Source column:** On **Linux X11**, the label comes from the X11 clipboard selection owner (`/proc/…/comm` or `WM_CLASS` after walking window parents). On **Wayland**, a source label is usually **not** available (the column stays “—”). On **Windows**, it uses the executable base name of the clipboard owner process. On **macOS**, it uses the **frontmost application** at poll time as a heuristic (not always the app that performed the copy).
- **Linux:** clipboard access uses GTK 3 (`GtkClipboard`), consistent with libui-ng on X11 and typical Wayland sessions (for example GNOME). Some Wayland compositors or sandboxed environments may restrict clipboard access; behavior can differ from X11.
- **System tray (Phase 2):** The Linux build uses GTK’s deprecated `GtkStatusIcon`. On **GNOME** and some **Wayland** setups there is **no legacy tray**, so the icon may not appear even though the process keeps running after you close the window; use the running process list or a desktop-specific extension if you need tray access. **Windows** and **macOS** use the native notification area / menu bar.
- **macOS / Windows:** basic polling and read/write of Unicode text; edge cases and permissions follow each platform’s clipboard rules.

### Building

From the repository root:

```sh
make
```

This produces `build/clipboard-manager` and builds libui-ng inside the submodule. On Linux, the shared library is written to `vendors/libui-ng/build/meson-out/libui.so` (Meson `layout=flat`).

**Optional:** link against a system-installed libui-ng instead of building the submodule:

```sh
make USE_SYSTEM_LIBUI=1 LIBUI_PREFIX=/usr/local
```

**Cleaning up:**

```sh
make clean       # remove the application build directory
make distclean   # like clean, and remove vendors/libui-ng/build
```

## Installation

