---
title: Download and Manage
description: Download Workshop wallpapers concurrently, view live speed, cancel tasks, and manage installed content.
---

## Starting a download

Start from an item card or detail view. Mirage uses the authenticated Steam session to resolve the item manifest and downloads it into Mirage's own wallpaper directory. The Steam account must own Wallpaper Engine. A Steam Web API Key is not required for downloads.

The embedded service calls SteamKit2 manifest and CDN APIs directly. Its server selection, chunk scheduling, validation, and resumable-reuse design is informed by DepotDownloader, while the actual tasks run in Mirage's own service without the DepotDownloader command-line application.

## Live progress

The download manager shows:

- Received bytes and total compressed size
- Completion percentage
- Smoothed live speed over the last few seconds
- Estimated time remaining
- Resolving, downloading, validating, and completed states

Speed comes from bytes actually received from Steam CDN HTTP response streams, not disk writes or the size advertised on the item page.

## Concurrency and cancellation

Mirage handles up to three items at once, with a global limit of eight chunk requests. A single item can use all available chunk connections; concurrent items share that limit. Each item has its own cancellation token; cancelling one does not terminate the Steam session or another download.

When a Steam connection is interrupted temporarily, Mirage keeps the staged content and automatically restores the session and active downloads. Re-login is required only when Steam explicitly rejects the saved session.

## Validation and installation

Files are written under `Workshop/content/431960/.staging` first. Mirage validates every manifest chunk and requires a root `project.json`, then atomically replaces the final item directory on the same volume. Failed or cancelled work never enters the wallpaper library, while reusable staged chunks remain available for a retry.

## Updating an item

When an installed item is downloaded again, Mirage validates the existing files and requests only missing or changed chunks. The wallpaper library refreshes automatically after installation.

## Bulk download previously subscribed wallpapers

Mirage's embedded downloader handles tasks by item ID and currently does not enumerate and import an account's complete subscription list. If you previously subscribed to many Wallpaper Engine Workshop items, you can optionally run Valve's standalone [SteamCMD](https://developer.valvesoftware.com/wiki/SteamCMD) to let Steam fetch Wallpaper Engine and the subscribed content for that account in one pass.

SteamCMD is not a Mirage component and is never installed, launched, or managed by Mirage. The following workflow runs entirely outside Mirage.

### Requirements

- Use a Steam account that owns Wallpaper Engine and is subscribed to the target wallpapers.
- Reserve enough space for the Windows version of Wallpaper Engine and all subscribed content. The actual size depends on the subscription list.

### Install standalone SteamCMD

Install it with Homebrew:

```bash
brew install --cask steamcmd
```

Alternatively, install it from Valve's official archive:

```bash
mkdir -p "$HOME/SteamCMD"
cd "$HOME/SteamCMD"
curl -fsSL "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_osx.tar.gz" | tar -xz
```

### Sign in and fetch subscriptions

Start SteamCMD with a dedicated install directory. For the Homebrew version, run:

```bash
steamcmd +force_install_dir "$HOME/Steam/WallpaperEngine"
```

For the manually installed version, run:

```bash
"$HOME/SteamCMD/steamcmd.sh" +force_install_dir "$HOME/Steam/WallpaperEngine"
```

In the interactive console, run these commands in order:

```ansi
Steam> login your_account_name
Steam> @sSteamCmdForcePlatformType windows
Steam> app_update 431960 validate
Steam> quit
```

The login command requests the password interactively instead of placing it in the command line. If Steam Guard is enabled, enter the requested code or approve the request in the Steam mobile app. `app_update` downloads Wallpaper Engine and the Workshop content already subscribed by that account; larger subscription lists take more time and disk space.

### Add the directory to Mirage

After the download completes, the items are normally stored under:

```text
~/Steam/WallpaperEngine/steamapps/workshop/content/431960/
```

Open **Mirage Settings → General**. Under wallpaper directories, select **Choose Directory...** for the Steam Workshop source and choose the `431960` directory above. After Mirage refreshes the library, the items appear under **Installed**.

When subscriptions change or need updating, run SteamCMD again, execute `app_update 431960 validate`, and refresh the Mirage library. Mirage's own Workshop downloads continue to use the embedded SteamKit service and remain independent from this external directory.

## Presets and dependencies

A preset depends on a base wallpaper. When the dependency is missing, Mirage asks for confirmation, queues the base item, and applies the preset after the dependency is ready. See [Presets and Dependencies](/en/workshop/presets/).

See [Data Directories](/en/advanced/data-directories/) for the on-disk paths.
