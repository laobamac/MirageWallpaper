---
title: Apply & preview
description: Select a wallpaper, preview it, and set it as your desktop wallpaper.
---

Once you select a work in the wallpaper library, you can preview it and apply it to your desktop.

## Preview and selection

Click a wallpaper card to select it, and Mirage loads its preview and properties panel. While selected, you can adjust the [playback parameters and custom properties](/en/wallpapers/playback/) before deciding whether to apply it for real.

## Apply to the desktop

Once you set the selected wallpaper as your current desktop wallpaper, the corresponding renderer process starts and takes over the desktop:

- **Scene wallpapers** are rendered by SceneWallpaper (Vulkan / MoltenVK).
- **Web wallpapers** are rendered by WebWallpaper (WKWebView).
- **Video wallpapers** are rendered by VideoWallpaper (AVFoundation).

Mirage remembers the current wallpaper and, when applying it, also sets a **desktop placeholder image** so the desktop doesn't suddenly go blank while the renderer process isn't running (for example, early in login).

## Cover all displays

By default, a wallpaper is applied to the primary display. To make the current wallpaper fill every display, use **Cover all displays** (triggered from either the main window or the [menu bar](/en/wallpapers/menubar/)). To assign different wallpapers to different displays, see [Multiple displays](/en/wallpapers/displays/).

## Stop wallpaper

**Stop wallpaper** shuts down all renderer processes and clears the current wallpaper. After that, the desktop returns to a static state until you apply a wallpaper again.

## Security confirmation for web wallpapers

When applying a **web wallpaper** from an untrusted source, Mirage shows a security prompt first, because web wallpapers execute JavaScript. Only trust web wallpapers from reliable sources. See [Web wallpaper safety](/en/settings/web-safety/) for details.

## About unsupported wallpapers

If a work's type can't be recognized or isn't supported, Mirage won't start a renderer process. Compatibility with the scene format is still being refined, and complex Wallpaper Engine scenes may differ in how they render.
