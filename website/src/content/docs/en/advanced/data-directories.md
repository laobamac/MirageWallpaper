---
title: Data Directories
description: Where Mirage keeps wallpaper source directories, the SteamCMD directory, caches, and settings.
---

Mirage scans wallpapers from several sources and keeps downloads, caches, and settings in separate locations. Knowing these paths helps with backing up, cleaning up, and troubleshooting.

## Wallpaper Source Directories

Mirage's wallpaper library scans all of the following sources at once (you can view the real paths and "Show in Finder" in the "Wallpaper library" section of [General Settings](/en/settings/general/)):

| Source | Description |
| --- | --- |
| Steam Workshop directory | The system Steam's default Workshop content directory (App ID `431960`) |
| Custom Workshop directory | A Wallpaper Engine content directory you specify manually (once set, the system default directory is demoted to a compatibility fallback) |
| Mirage download directory | Where SteamCMD currently downloads and updates wallpapers (marked "current download location") |
| Mirage legacy download directory | Only used for compatibility with wallpapers downloaded by older versions |
| Import directory | Local wallpapers imported manually or created from videos |

Only subdirectories whose **root contains `project.json`** are recognized as wallpapers.

## SteamCMD Directory

The SteamCMD that Mirage manages uses a dedicated directory under the application support directory:

```text
~/Library/Application Support/Mirage/steamcmd/
├── .mirage-ready                       # installation-complete marker
├── home/                               # isolated HOME
│   └── Library/Application Support/Steam/
│       ├── config/config.vdf           # SteamCMD session
│       └── steamapps/workshop/content/431960/   # current download directory
└── steamapps/workshop/content/431960/  # legacy download directory (compatibility)
```

This directory is accessible only to the current user (`0700`). See [SteamCMD and Steam Login](/en/workshop/steamcmd/) for details.

## Cache

Thumbnails and data from Workshop browsing are cached in the cache directory:

```text
~/Library/Caches/Mirage/WorkshopCache/
```

Clearing the cache does not affect downloaded wallpapers; it only makes the next browse re-fetch the data.

## Screen Saver Component

The live screen saver is installed into the current user's screen saver directory:

```text
~/Library/Screen Savers/MirageScreenSaver.saver
~/Library/Application Support/Mirage/screensaver.json   # screen saver configuration
```

See [Live Screen Saver](/en/screensaver/overview/).

## Settings and Runtime State

- **Global settings**, trusted web wallpapers, the SteamCMD username, and the like are stored in the app's `UserDefaults` (preferences).
- **Per-wallpaper runtime state** (volume, speed, fill, custom properties, etc.) is stored in `UserDefaults` under keys of the form `Runtime_<id>`.

## Custom Directories

In [General Settings](/en/settings/general/), you can set custom locations for the "Steam Workshop directory" and the "Import directory", and you can "Restore Defaults" at any time.

:::caution[Clean up with care]
Deleting the download directory or the import directory removes the corresponding wallpapers; deleting the SteamCMD directory means you'll need to reinstall and log in again. Confirm a directory's contents before backing it up.
:::
