---
title: SteamCMD and Steam Sign-In
description: How Mirage manages SteamCMD, isolates its data directory, and handles Steam sign-in and credentials.
---

Mirage uses **SteamCMD**, Valve's official command-line tool, to download Workshop content. The whole process is managed by Mirage, so you normally don't need to touch the command line yourself.

## Installation and detection

In the [setup wizard](/MirageWallpaper/en/workshop/setup-wizard/), Mirage will:

1. First **detect** whether a usable SteamCMD is already present.
2. If not, download the official `steamcmd_osx.tar.gz` from `steamcdn-a.akamaihd.net` and install it to a dedicated directory.

SteamCMD is installed under the application support directory at `Mirage/steamcmd`, with access restricted to the current user only (permissions `0700`). Once installation finishes, a `.mirage-ready` marker is written so later launches can identify it quickly.

## Isolated data directory

To avoid interfering with any Steam installation already on your system, Mirage has SteamCMD use a **separate HOME and data directory**:

- SteamCMD home: `Mirage/steamcmd`
- Isolated Steam data: `Mirage/steamcmd/home/Library/Application Support/Steam`

Workshop content (App ID `431960`) is downloaded to `steamapps/workshop/content/431960` under that dedicated directory. Mirage's wallpaper library scans the current download directory, the legacy download directory, and the system Steam's default directory together; for the full list, see [Data Directories](/MirageWallpaper/en/advanced/data-directories/).

## Steam sign-in

Downloading Workshop content requires a signed-in Steam account.

- You enter your username and password in the wizard, and Mirage hands them to SteamCMD to complete sign-in; the password is not stored in plain text long-term.
- Once sign-in succeeds, SteamCMD writes its own session config (`config/config.vdf`) in the isolated directory. Mirage uses this to determine whether you're signed in, and reuses the session automatically when downloading.
- Mirage only persists your **username**, to reuse the session next time; the session itself is maintained by SteamCMD.

## Steam Guard

If your account has Steam Guard enabled:

- **Code method**: the wizard prompts you to enter the email / token code.
- **Mobile confirmation method**: Mirage enters a waiting state and prints a "still waiting" note in the log every 5 seconds; just confirm in the Steam mobile app.

## Session reuse and expiry

When a valid session already exists on this machine, the wizard offers "Use saved session" so you don't have to re-enter your password. If the session expires or becomes invalid, Mirage prompts you to sign in again.

## Privacy and redaction

Mirage keeps a **redacted** support diagnostic log for troubleshooting Workshop issues. The log automatically hides sensitive fields like passwords, API keys, and tokens (replacing them with `[redacted]`), and retains the most recent events. You can export this report for feedback; see [Troubleshooting](/MirageWallpaper/en/workshop/troubleshooting/) for details.

:::caution
Only sign in to your own Steam account on your own device, and keep your credentials secure. Mirage never uploads your password to any third-party server; sign-in happens only in the local SteamCMD process.
:::
