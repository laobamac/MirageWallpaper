---
title: Live Screen Saver
description: Set a live wallpaper as your macOS screen saver, running independently of the main Mirage app.
---

Mirage provides a standalone **live screen saver** component that lets you set a video, web, or scene wallpaper as your macOS screen saver. The screen saver is loaded by Mirage's own host, and **does not require the main Mirage app to keep running**.

## Installing the screen saver component

In "Settings → Screen Saver":

1. Click **Install**. Mirage copies the screen saver component to the current user's `~/Library/Screen Savers/MirageScreenSaver.saver`.
2. After installing, click **Open System Screen Saver Settings** and select Mirage as your screen saver in System Settings.

Once installed, the button changes to **Reinstall** and an **Uninstall** option appears. Because the screen saver is copied out of the app bundle, updating Mirage.app on its own doesn't automatically update the installed screen saver. Click "Reinstall" when you want to sync it to the latest version.

## Choosing the screen saver wallpaper

On the same page:

- **Current screen saver wallpaper**: shows the wallpaper set as your screen saver.
- **Now playing**: shows the current desktop wallpaper.
- **Set the now-playing wallpaper as screen saver**: configures the current desktop wallpaper as the screen saver content.

The configuration saves the wallpaper's preset, custom properties, fill mode, and a frame rate of up to 60 FPS. Later customizations to the same wallpaper sync to the screen saver automatically.

## How the screen saver runs

- **Muted**: the screen saver always plays muted.
- **Frame rate cap**: up to 60 FPS.
- **Web screen savers**: they get no network navigation permission, and audio response stays muted in the screen saver environment, for safety and stability.
- **Runs independently**: video, web, and scene wallpapers are all loaded by Mirage's screen saver host, with no need for the main app to run in the background.

## Uninstalling

Click **Uninstall** in "Settings → Screen Saver" to remove the screen saver component. This deletes the installed `.saver` but doesn't affect your wallpaper library.

## Related

- For a per-item explanation of the screen saver settings, see [Screen Saver settings](/en/settings/screensaver/).
- For how the screen saver host loads different wallpaper types, see [Render architecture](/en/advanced/architecture/).
