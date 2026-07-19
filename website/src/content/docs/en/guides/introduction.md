---
title: What is Mirage
description: Meet Mirage — a native live wallpaper manager for macOS and a Wallpaper Engine compatible runtime.
---

Mirage is a **native live wallpaper manager** for macOS, and also a **Wallpaper Engine compatible runtime**. Built with SwiftUI and AppKit, it handles wallpaper browsing, management, and system integration, while three separate renderer processes play scene, web, and video wallpapers.

You can use it to read local Wallpaper Engine style wallpaper packages, or browse the Steam Workshop directly, install SteamCMD, sign in to Steam, and download wallpapers.

:::note[The project is still in an early stage]
Mirage is under active development, and compatibility with the Wallpaper Engine scene format is still being refined. Complex works may differ in how they render effects, scripts, or materials. If you run into problems, feel free to [open an issue](https://github.com/laobamac/MirageWallpaper/issues/new/choose) or join the community to share feedback. See [Community & Feedback](/en/reference/community/) for details.
:::

## What it can do

- **Play three kinds of live wallpapers**: `scene`, `web`, and `video`.
- **Manage your wallpaper library**: browse installed wallpapers with search, sorting, favorites, and filtering by type, source, tag, and content rating.
- **Import and convert**: import a directory containing a `project.json` directly, or convert `.mp4`, `.mov`, and `.m4v` videos into local wallpaper packages.
- **Connect to the Steam Workshop**: browse trending, latest, popular, top-rated, and tag-based content, and recognize and download Workshop presets.
- **Host SteamCMD**: Mirage manages the SteamCMD install, its own dedicated data directory, Steam sign-in, and Steam Guard verification.
- **System integration**: multi-display coverage, menu bar control, launch at login, and desktop placeholder image recovery.
- **Live screen savers**: package video, web, and scene wallpapers into standalone screen savers that keep the current preset and custom properties.
- **Smart power saving**: continue, mute, pause, or stop playback when a full-screen app is active, another app is playing audio, the screen sleeps, or the Mac is on battery power.

## Core design: multi-process rendering

Mirage delegates rendering to three separate processes, leaving the main app to handle the interface and coordination:

| Component | Technology | Responsibility |
| --- | --- | --- |
| Mirage | SwiftUI, AppKit | Interface, wallpaper library, Workshop, settings, process management, and system integration |
| SceneWallpaper | C++20, Vulkan, MoltenVK | `scene.pkg` / `scene.json`, materials, particles, LUTs, text, and user properties |
| WebWallpaper | Objective-C++, WKWebView | HTML wallpapers, JavaScript, media, mouse events, and user properties |
| VideoWallpaper | Objective-C++, AVFoundation | Video looping, volume, speed, and fill mode |
| MirageScreenSaver | Swift, WebKit, AVFoundation, Metal | Standalone-installed host for live screen savers |

The renderers run as separate processes. Mirage sends line-by-line JSON control messages over standard input, so a crash in a single renderer won't directly corrupt the main app's state. To dig deeper, read [Rendering architecture](/en/advanced/architecture/).

## What's next

- Start by confirming your machine meets the [system and build requirements](/en/guides/requirements/).
- Then get Mirage running with [Install & first launch](/en/guides/install/).
- To get familiar with the interface quickly, see the [Interface tour](/en/guides/interface/).

:::caution[Not officially affiliated]
Mirage is not affiliated with or endorsed by Valve, Steam, or Wallpaper Engine. When using Steam Workshop content, please follow the applicable licenses and terms.
:::
