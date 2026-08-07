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

## Bulk pull subscribed wallpapers

If you've already subscribed to a large number of wallpapers in Wallpaper Engine, you can pull all of them at once through SteamCMD instead of searching and downloading them one by one.

### Prerequisites

- A **Steam account** that has subscribed to wallpapers in the Wallpaper Engine Workshop
- This process downloads Wallpaper Engine itself (about 1 GB) along with all your subscribed Workshop content

### Step 1: Install the Steam client and sign in

Go to the [Steam website](https://store.steampowered.com/) to download and install the macOS Steam client, then sign in with the Steam account you use for wallpapers. This ensures your subscriptions are tied to your account so steamcmd can recognize them.

### Step 2: Install steamcmd

:::tip
Mirage automatically downloads and installs steamcmd the first time you use the Workshop. However, if you prefer to **manage a dedicated steamcmd instance** to keep wallpaper sources separate, follow the instructions below to install it manually.
:::

**Option A: Homebrew**

```bash
brew install steamcmd
```

**Option B: Manual download**

```bash
mkdir -p ~/Steam && cd ~/Steam
curl -sqL "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_osx.tar.gz" | tar zxvf -

# Add to PATH
echo 'export PATH="$PATH:$HOME/Steam"' >> ~/.zshrc
source ~/.zshrc
```

### Step 3: Sign in to steamcmd and pull

```bash
steamcmd
```

Inside the interactive console, run:

```ansi
Steam> login your_username your_password
```

If your account has Steam Guard enabled:

- **Code method**: append the code to the command: `login username password code`
- **Mobile confirmation**: leave out the code and approve the sign-in from the Steam mobile app

Once signed in, set the platform type and start downloading:

```ansi
Steam> @sSteamCmdForcePlatformType windows
Steam> app_update 431960 validate
```

:::caution
The download may take a long time — steamcmd pulls Wallpaper Engine itself plus **every Workshop wallpaper you've subscribed to**. Duration depends on your subscription count and network conditions.
:::

Type `quit` when finished to exit steamcmd.

### Step 4: Open Mirage

Launch Mirage. The wallpaper library automatically scans the steamcmd download directories for Workshop content. All your subscribed wallpapers will appear under the **Installed** tab and can be applied immediately.

If wallpapers don't show up:
- Verify the steamcmd download path. See [Data Directories](/en/advanced/data-directories/) for the paths Mirage scans by default
- Go to **Mirage Settings → General** and check or update the "steamcmd Workshop path"

### Ongoing maintenance

- **Daily use**: Mirage's built-in Workshop browsing and downloading remains fully available and works independently of the manual pull method
- **Updating wallpapers**: the Steam client can automatically update subscribed content; you can also re-run `app_update 431960 validate` in steamcmd to pull the latest versions
- **New subscriptions**: after subscribing to new wallpapers in the Steam client (or Wallpaper Engine), repeat Step 3 to sync them to Mirage

## If you run into problems

If a download fails, stalls, or sign-in acts up, first check your network and Steam sign-in status, then see [Troubleshooting](/en/workshop/troubleshooting/). Mirage provides a redacted diagnostic report you can use for feedback.
