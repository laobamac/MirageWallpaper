---
title: Troubleshooting
description: Fix common Mirage problems - wallpapers not showing, performance issues, failed imports, and update problems.
---

This page collects common problems you may run into while using Mirage. For Workshop-related login and download problems, see [Workshop Troubleshooting](/en/workshop/troubleshooting/).

## Wallpaper Not Showing or Black Screen

- Confirm that the wallpaper directory has a `project.json` at its root and that the type is supported (scene / web / video).
- If the wallpaper was just imported or downloaded, try **refreshing** in the wallpaper library.
- Complex Wallpaper Engine scenes may have compatibility differences; try another wallpaper to check whether it's an isolated case.
- A web wallpaper needs you to **confirm trust** in the security prompt the first time it's applied, otherwise it won't show. See [Web Wallpaper Safety](/en/settings/web-safety/).

## Wallpaper Stutters or Uses a Lot of Resources

- In [Performance Settings](/en/settings/performance/), lower the frame rate (30 is recommended) or reduce the render quality.
- Turn off anti-aliasing.
- Scene wallpapers generally use more resources than video or web wallpapers.

## Wallpaper Pauses or Stops Unexpectedly

This is usually a **playback rule** taking effect, which is expected power-saving behavior. Check the rules in [Performance Settings](/en/settings/performance/):

- When another app is full-screen / focused / playing audio;
- When the display sleeps;
- When a laptop is on battery power.

When multiple conditions match at once, the strongest action wins (stop > pause > mute > keep running).

## Multi-Display Issues

- Each screen can have its own wallpaper, so make sure you're operating on the target screen.
- After plugging or unplugging a display or changing the arrangement, refresh the wallpaper library or reapply. See [Multiple Displays](/en/wallpapers/displays/) for details.

## Import Fails

- Confirm the video is a common format (`.mp4` / `.mov` / `.m4v`).
- Confirm the import directory is writable; you can view or change the import directory in [General Settings](/en/settings/general/).
- See [Import Local Wallpapers](/en/formats/import/) for details.

## Sound Issues

- Check the global volume and global mute in [General Settings](/en/settings/general/).
- Check the wallpaper's own [volume settings](/en/wallpapers/playback/).
- The screen saver is always muted by design.

## Update Problems

- If automatic updates are off, check manually with "Check for Updates…" in the menu bar.
- The beta channel must be enabled in [Software Update Settings](/en/settings/updates/).
- A first-time install may need you to allow Mirage manually in macOS Gatekeeper.

## Screen Saver Not Working

- In [Screen Saver Settings](/en/settings/screensaver/), confirm the component is **installed**.
- After installing, you still need to select Mirage in the **System Settings** screen saver pane.
- If the screen saver misbehaves after updating Mirage, click "Reinstall" to sync it to the latest version.

## Collecting Diagnostic Information

- Enable "Verbose logging" in [General Settings](/en/settings/general/) for more information.
- For Workshop issues, export a redacted support report.
- When reporting a problem, include your system version, Mirage version, and steps to reproduce; see [Community & Feedback](/en/reference/community/).
