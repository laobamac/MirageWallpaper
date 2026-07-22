---
title: Screen Saver Settings
description: A per-item explanation of the options on the "Settings → Screen Saver" page.
---

"Settings → Screen Saver" is where you install, configure, and manage Mirage's [live screen saver](/en/screensaver/overview/). This page explains the contents of that settings page item by item.

## Screen saver component

The top block shows the installation status of the screen saver component:

- **Status indicator**: "Mirage live screen saver installed" or "Not installed".
- **Install / Reinstall**: copies the screen saver component to `~/Library/Screen Savers/MirageScreenSaver.saver`. When already installed, it shows "Reinstall", used to update to the version matching the current Mirage.
- **Uninstall**: removes the installed screen saver component (a destructive action that deletes the `.saver`).
- **Open System Screen Saver Settings**: jumps to System Settings, where you need to manually select Mirage as your screen saver. This button is available once installed.

While installing or uninstalling, an "Updating screen saver component…" progress indicator is shown.

:::note
Mirage installs the screen saver to the **current user's** `Library/Screen Savers` directory. After installation, you still need to actively select Mirage in the screen saver panel of System Settings for it to take effect.
:::

## Screen saver wallpaper

The middle block manages what the screen saver plays:

- **Current screen saver wallpaper**: the title of the wallpaper configured as the screen saver (shows "Not selected" when unconfigured).
- **Now playing**: the title of the current desktop wallpaper.
- **Set the now-playing wallpaper as screen saver**: sets the current desktop wallpaper as the screen saver content. Unsupported types are unavailable.

The configuration saves: preset, custom properties, fill mode, and a frame rate of up to 60 FPS. The screen saver is **always muted**. Later customizations to the same wallpaper sync to the screen saver automatically.

## How it runs

The bottom explains how the screen saver runs: video, web, and scene wallpapers are all loaded by Mirage's own screen saver host, and **do not require the main Mirage app to keep running**. Web screen savers get no network navigation permission, and audio response stays muted in the screen saver environment.

## Operation failures

If installing, uninstalling, or configuring fails, Mirage shows a "Screen saver operation failed" alert with the specific reason. Common causes include insufficient permissions or a target directory that isn't writable.
