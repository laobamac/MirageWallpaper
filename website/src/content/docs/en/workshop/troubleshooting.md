---
title: Workshop Troubleshooting
description: Resolve common issues with Workshop browsing, sign-in, and downloading.
---

The Workshop feature spans several layers: the network, the Steam Web API, and SteamCMD. Below are common problems organized by symptom, along with how to handle them.

## Browsing

**Keeps showing busy, times out, or content won't load**

- Usually the shared built-in Steam Web API Key is rate-limited. Adding your own [Steam Web API Key](/en/workshop/api-key/) generally fixes it.
- When the official endpoint is unstable on your network, switch to the **mirror endpoint** in [Settings](/en/settings/general/).

**"API Key is invalid"**

- Confirm the Key is a 32-character hexadecimal string with no extra spaces. If the format is invalid, Mirage falls back to the built-in Key.

## Sign-in

**Sign-in fails**

- Double-check your username and password.
- If Steam Guard is enabled, note the difference between the code method and the mobile confirmation method: mobile confirmation requires tapping confirm in the Steam mobile app.
- Check the sign-in log in the wizard, which usually has the specific reason.

**Mobile confirmation keeps waiting**

- Mirage prints a "still waiting" note every 5 seconds. Complete the confirmation in the Steam mobile app; if there's no response for a long time, cancel and retry.

**"Session is no longer valid"**

- Once a saved session expires, you need to sign in again with your password. Just re-run the [setup wizard](/en/workshop/setup-wizard/).

## SteamCMD

**SteamCMD not detected**

- Let Mirage install it automatically. The install downloads from Valve's official address and deploys to a dedicated directory.

**Installation fails**

- Check whether your network can reach `steamcdn-a.akamaihd.net`.
- You can cancel and retry the installation.

## Downloading

**Download stalls or fails**

- Confirm your Steam sign-in is still valid.
- Check your network connection.
- Cancel the task and download again; re-downloading pulls the latest version.

**Preset won't display**

- A preset needs its base dependency work. See [Presets](/en/workshop/presets/) and let Mirage download the dependency automatically.

## Exporting a diagnostic report

Mirage keeps a **redacted** Workshop support report that automatically hides sensitive fields like passwords, API keys, and tokens. You can attach this report when reporting an issue to help with diagnosis. For feedback channels, see [Community and Feedback](/en/reference/community/).

## Where to find related settings

- API Key and endpoint: [General and Audio Settings](/en/settings/general/)
- Sign-in and SteamCMD: [SteamCMD and Steam Sign-In](/en/workshop/steamcmd/)
- Data and cache locations: [Data Directories](/en/advanced/data-directories/)
