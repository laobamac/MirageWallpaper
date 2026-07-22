---
title: Download and Manage
description: Download wallpapers from the Workshop, track download progress, cancel tasks, and manage downloaded content.
---

Once you've found a wallpaper you like in the Workshop, Mirage uses its managed SteamCMD to download it to your local wallpaper library.

## Start a download

Kick off a download from an item card or its detail page. Mirage uses the signed-in SteamCMD session to download the corresponding Workshop item (App ID `431960`) to an isolated download directory.

Downloading requires [SteamCMD and Steam Sign-In](/en/workshop/steamcmd/) to be set up first. If you're not signed in or the session has expired, Mirage prompts you to complete setup first.

## Download progress and badge

In-progress downloads show their status. The Workshop tab at the top displays a **badge** with the number of active downloads, so you can keep an eye on progress at any time.

## Parallel downloads and cancellation

Mirage supports multiple download tasks. You can **cancel** an in-progress download at any time; cancelled tasks are marked and won't clutter your wallpaper library.

## After a download completes

Completed wallpapers go into your local wallpaper library, where you can browse, filter, and apply them from the Installed tab. They're grouped under the Workshop source; for their actual paths, see [Data Directories](/en/advanced/data-directories/).

## Presets and their dependencies

If what you downloaded is a **preset**, it needs a base work to display correctly. When you try to apply a preset that's missing its dependency, Mirage pops up a prompt and helps you download the required base work first, then applies the preset once the dependency is ready. See [Presets](/en/workshop/presets/).

## Updates and re-downloading

Workshop content may get updated. Re-downloading the same item makes SteamCMD pull the latest version. You can also delete downloads you no longer need from the wallpaper library to free up space.

## If you run into problems

If a download fails, stalls, or sign-in acts up, first check your network and Steam sign-in status, then see [Troubleshooting](/en/workshop/troubleshooting/). Mirage provides a redacted diagnostic report you can use for feedback.
