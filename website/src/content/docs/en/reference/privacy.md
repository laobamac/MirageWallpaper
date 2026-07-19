---
title: Privacy & Data
description: How Mirage handles your Steam credentials, API Key, and local data.
---

Mirage is a locally running desktop app. It does not operate any account system, and it does not upload your usage data to Mirage's servers. This page covers the key privacy-related points.

## Steam Credentials

- When you log in to Steam, your username and password are handed only to the **local SteamCMD process** to complete the login, and are **never uploaded to any third-party server**.
- Your password is not kept in plain text long-term; after a successful login, SteamCMD maintains its own session in a local isolated directory.
- Mirage only persists your **username**, to reuse the session next time.
- Steam Guard codes are likewise used only for the current login.

See [Steam Login](/en/workshop/login/) and [SteamCMD](/en/workshop/steamcmd/) for details.

## Steam Web API Key

- The API Key you enter is stored in local settings and is used only to **request Workshop browsing data from your own machine**.
- Do not share your key publicly.
- The app ships with a built-in key shared by all users, for browsing only.

See [Steam Web API Key](/en/workshop/api-key/).

## Network Requests

Mirage makes network requests in a limited and predictable set of cases:

| Purpose | Target |
| --- | --- |
| Browse the Workshop | Steam Web API (the official endpoint or a mirror you choose) |
| Download Workshop content | Steam, via SteamCMD |
| Install SteamCMD | Valve's official CDN |
| Check / download updates | Mirage's update feed (GitHub Release / appcast) |
| Web wallpapers | Determined by the wallpaper itself (see below) |

### Mirror Endpoints

The optional SteamCF mirror available to users in mainland China is **not an official Steam service**. It offers no guarantee of security or availability, and it only proxies the browsing API — it does not accelerate logins or downloads. Whether to use it is up to you.

## Web Wallpapers

Web wallpapers execute JavaScript and may make their own network requests or load remote resources. That's why Mirage has a [security confirmation](/en/settings/web-safety/) for untrusted web wallpapers. Only trust web wallpapers from reliable sources.

## Diagnostic Logs

- Mirage's Workshop support reports are **redacted automatically**: fields such as passwords, API Keys, and tokens are replaced with `[hidden]`.
- Enabling "Verbose logging" records more information for debugging; before you export or share logs, please confirm for yourself that they contain nothing sensitive.

## Local Data

Your wallpapers, downloads, caches, and settings are all stored on your own machine. For the relevant paths, see [Data Directories](/en/advanced/data-directories/). You can view, back up, or clean up these directories in Finder at any time.

## Affiliation Notice

Mirage is not affiliated with, nor endorsed by, Valve, Steam, or Wallpaper Engine. Workshop content belongs to its respective authors; please observe the applicable licenses and terms of use.
