# Linux X11 Wallpaper Host

`SceneWallpaper` supports a Linux MVP on X11. The renderer still uses the normal
Vulkan surface and swapchain path; the platform host only owns the X11 window,
desktop placement, monitor geometry, and mouse polling.

## Requirements

- CMake 4.3.1 or newer. CMake 4.4.0 is known to work.
- Clang 20 or newer with C++20 module support.
- Ninja.
- Vulkan loader, headers, and a working Vulkan driver.
- X11 development packages for `X11`, `Xrandr`, `Xfixes`, and `Xext`.

Example build with an explicit CMake executable:

```bash
scripts/build.sh --cmake /home/elysia/Projects/Wallpapers/cmake-4.4.0/bin/cmake release
scripts/build.sh --cmake /home/elysia/Projects/Wallpapers/cmake-4.4.0/bin/cmake debug
```

Use `KEEP_GOING=1` to pass `-k 0` to Ninja and keep compiling after independent
errors:

```bash
KEEP_GOING=1 scripts/build.sh --cmake /home/elysia/Projects/Wallpapers/cmake-4.4.0/bin/cmake build
```

## Placement Strategy

The X11 host chooses a placement strategy in this order:

1. Reuse an existing viewable `_NET_WM_WINDOW_TYPE_DESKTOP` window that
   intersects the selected monitor, and create the Vulkan window as its child.
2. If an EWMH window manager is present, create a top-level window with desktop
   window type and desktop-friendly hints.
3. If no EWMH window manager is detected, create an override-redirect child of
   the root window and lower it.

The host sets desktop-type, borderless, sticky, below, skip-taskbar, skip-pager,
and all-desktops hints where the window manager supports them.

## Monitor Selection

`--screen N` maps to the active XRandR monitor index returned by
`XRRGetMonitors`. Indexing is zero-based. If the index is invalid, startup fails
and prints the available monitor count and geometries.

If XRandR monitors cannot be queried, the host falls back to root-window
geometry and only `--screen 0` is accepted.

All window geometry and normalized mouse coordinates are relative to the
selected monitor. The host subscribes to XRandR and configure notifications and
resizes/repositions the window when monitor geometry changes.

## Input

The window uses an empty XFixes input shape so desktop clicks pass through to
the real desktop. Mouse position and button state are still polled from the root
window, so interactive wallpapers continue to receive normalized pointer input.

If XFixes is unavailable, startup continues but the window cannot be guaranteed
click-through.

## Wayland

Native Wayland sessions are rejected by default because desktop-layer wallpaper
integration is compositor-specific.

`SCENERENDERER_FORCE_X11=1` forces the X11 host to run under XWayland for
debugging only. It does not promise correct desktop-layer behavior under a
Wayland compositor.

Wayland support is a separate phase:

- KDE Plasma: implement the Plasma wallpaper protocol path.
- wlroots compositors: implement a layer-shell backend.
- GNOME Wayland: implement a GNOME Shell extension integration.
