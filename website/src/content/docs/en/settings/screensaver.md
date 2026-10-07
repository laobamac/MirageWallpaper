---
title: Screen Saver Settings
description: Screen saver component, screen saver wallpaper, and dynamic lock-screen options in Settings.
---

**Settings → Screen Saver** installs, configures, and manages Mirage's [live screen saver](/en/screensaver/overview/).

## Screen saver component

- **Install / Reinstall** copies `MirageScreenSaver.saver` to `~/Library/Screen Savers/` for the current user.
- **Uninstall** removes the installed `.saver` without affecting the wallpaper library.
- **Open System Screen Saver Settings** opens macOS Settings; after installing, you still need to select Mirage there.

## Screen saver wallpaper

The **Set the now-playing wallpaper as screen saver** button is available when the current desktop wallpaper is a video or scene. The configuration saves its preset, custom properties, fill mode, and frame rate; the screen saver is always muted and capped at 60 FPS. Web and other unsupported types cannot be configured as the screen saver.

The screen saver uses an independent host, so the main Mirage app does not need to stay open. Custom changes to the same wallpaper are synchronized to the screen saver configuration.

## Dynamic lock screen

- Scheme A requires macOS 26 or later and uses an undocumented system API that may break after a system update.
- Scheme B requires macOS 14.2 or later, the Mirage screen saver component, and an undocumented system configuration format; it restores the previous desktop configuration after unlock.
- The schemes are mutually exclusive and support video and scene wallpapers only. Scheme B may require Full Disk Access with an unsigned development build.
