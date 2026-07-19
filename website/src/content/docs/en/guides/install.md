---
title: Install & first launch
description: Download or build Mirage, complete the first launch, and handle the Gatekeeper prompt.
---

There are two ways to get Mirage: download a ready-made release, or build it from source.

## Option 1: Download a release (recommended)

1. Open [GitHub Releases](https://github.com/laobamac/MirageWallpaper/releases).
2. Pick the build that matches your Mac's architecture (`x86_64` for Intel, `arm64` for Apple Silicon).
3. Once downloaded, drag `Mirage.app` into Applications.

Stable releases (`v*` tags) come from the stable update channel; rolling builds from the `main` branch are published as `prerelease` and use the beta update channel. Each architecture uses its own appcast, and both the update packages and release notes are signed with Sparkle Ed25519.

### Handling the Gatekeeper prompt

Because the current build uses ad-hoc signing, macOS may say it can't verify the developer the first time you open it. When that happens, go to System Settings → Privacy & Security, find the blocked Mirage at the bottom of the page, and click "Open Anyway". The authenticity of future updates is verified by the built-in Ed25519 public key and no longer relies on Gatekeeper.

## Option 2: Build from source

First make sure you meet the [build requirements](/en/guides/requirements/), then:

```bash
git clone https://github.com/laobamac/MirageWallpaper.git
cd MirageWallpaper

./SceneRenderer/scripts/build.sh release
./WebRenderer/scripts/build.sh release
./VideoRenderer/scripts/build.sh release
./Mirage/scripts/build.sh Release

open "Mirage/dist/Mirage.app"
```

The final app lives at `Mirage/dist/Mirage.app`. For full build instructions, Debug builds, and configuring a built-in Steam Web API Key, see [Build from source](/en/advanced/build/).

## First launch

After the first launch, we recommend doing a few things in order:

1. **Learn the interface**: the top tabs switch between the "Installed" wallpaper library and "Discover / Workshop". See the [Interface tour](/en/guides/interface/).
2. **Add wallpapers**: you can [import a local directory or video](/en/formats/import/) directly, or [set up the Steam Workshop](/en/workshop/overview/) and download.
3. **Enter your own Steam Web API Key** (optional but recommended): the built-in key is shared by all users and can get busy. See [Steam Web API Key](/en/workshop/api-key/).

:::tip
The app bundles a `MirageScreenSaver.saver` that you can install from Settings → Screen Saver. Once installed, you can use the live screen saver even without keeping Mirage running. See [Live screen saver](/en/screensaver/overview/).
:::

## Where data is stored

Mirage's local wallpapers, SteamCMD data, cache, and configuration all have fixed locations. For the full list, see [Data directories](/en/advanced/data-directories/).
