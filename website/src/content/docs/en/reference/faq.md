---
title: FAQ
description: Common questions about installing Mirage, wallpapers, the Workshop, performance, and updates.
---

## General

### Is Mirage free?

Yes. Mirage will continue to be developed freely and openly under the GPL-3.0 license. If it brings value to your desktop, you're welcome to sponsor it through the channels listed on the [community page](/MirageWallpaper/en/reference/community/) as you see fit; sponsorship is entirely voluntary and does not affect access to any features.

### Which systems does Mirage support?

An Intel (`x86_64`) or Apple Silicon (`arm64`) Mac running macOS 14.2 or later. See [System Requirements](/MirageWallpaper/en/guides/requirements/) for details.

### Is Mirage an official Steam client?

No. Mirage is not an official Steam client, and it is not affiliated with Valve or Wallpaper Engine. It browses the Workshop through Steam's official Web API and downloads content through SteamCMD, Valve's official command-line tool.

## Wallpapers

### Which wallpaper types does Mirage support?

Three: scene (`scene`), web (`web`), and video (`video`). Each wallpaper directory uses `project.json` as its entry point. See [Wallpaper Types](/MirageWallpaper/en/formats/wallpaper-types/) for details.

### Why do some Wallpaper Engine scenes not render correctly?

Mirage's compatibility with the Wallpaper Engine scene format is still improving. Complex wallpapers may show differences in effects, scripts, or materials. When you run into problems, feel free to open an Issue and include information about the wallpaper.

### Can I turn my own video into a wallpaper?

Yes. Import a `.mp4`, `.mov`, or `.m4v` file into Mirage, and it automatically generates the `project.json` and a preview image. See [Converting Video to Wallpaper](/MirageWallpaper/en/formats/video-convert/) for details.

### Are a wallpaper's volume and playback speed remembered?

Yes. Each wallpaper's volume, speed, mute state, and fill mode are saved individually and restored automatically the next time you apply it. See [Playback Controls](/MirageWallpaper/en/wallpapers/playback/) for details.

## Workshop

### What do I need to browse the Workshop?

A Steam Web API Key. The app ships with a shared key for first-time browsing, but we recommend [setting your own key](/MirageWallpaper/en/workshop/api-key/) to avoid the shared quota getting busy.

### Do I have to pay to download Workshop content?

You need a Steam account that owns Wallpaper Engine. Ownership and item access are verified by Steam on the first download. Mirage downloads through SteamCMD and does not need an API Key.

### Why do downloads happen one at a time?

A single interactive SteamCMD session can only reliably handle one Workshop command at a time, so the download queue currently runs serially. See [Downloading Wallpapers](/MirageWallpaper/en/workshop/download/) for details.

### What is a "preset"?

A preset only saves properties and accompanying assets, and depends on a base wallpaper. When the base wallpaper is already installed, clicking the preset applies it directly; when it isn't installed yet, Mirage asks whether to download it as well. See [Presets and Dependencies](/MirageWallpaper/en/workshop/presets/) for details.

## Performance

### Do live wallpapers drain a lot of power?

It depends on the wallpaper type, frame rate, and quality settings. In [Performance Settings](/MirageWallpaper/en/settings/performance/), you can have wallpapers automatically pause or stop to save resources when on battery power, when another app is full-screen, when the display sleeps, and so on.

### What frame rate should I set?

30 FPS is a balanced default. Going above 30 increases power usage, and going above 60 significantly increases both power usage and load.

## Updates and Screen Saver

### How does Mirage update?

It automatically checks for and downloads updates through Sparkle. You can turn off automatic updates or enable the beta channel in [Software Update Settings](/MirageWallpaper/en/settings/updates/).

### Does the screen saver require Mirage to be running?

No. The screen saver component is copied to `~/Library/Screen Savers` and runs independently. See [Screen Saver](/MirageWallpaper/en/screensaver/overview/) for details.
