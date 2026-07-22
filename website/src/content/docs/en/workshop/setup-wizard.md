---
title: Workshop Setup Wizard
description: The four-step first-run setup for the Workshop, from SteamCMD and Steam sign-in to completion.
---

The first time you switch to the Workshop tab, Mirage opens a four-step wizard that gets the Workshop feature ready. You can go back a step at any time, and any operation still in progress (installation, sign-in) is cancelled safely.

## Step 1: Welcome

Explains that the Workshop feature uses SteamCMD and Steam sign-in, and what they're for. You can continue straight from this step.

## Step 2: SteamCMD

Mirage first **detects** whether a usable SteamCMD is already present on your system:

- **Found**: uses the existing SteamCMD directly.
- **Not found**: Mirage **installs** it automatically. It downloads `steamcmd_osx.tar.gz` from Valve's official address and deploys it to a dedicated directory.

You can cancel during installation. You can only move to the next step once SteamCMD is detected or installation completes. For where SteamCMD is stored and how it's isolated, see [SteamCMD and Steam Sign-In](/en/workshop/steamcmd/).

## Step 3: Steam Sign-In

Downloading Workshop content requires signing in to a Steam account.

- Enter your **Steam username and password** to sign in. The password is only used to complete sign-in with SteamCMD and is never stored in plain text.
- If your account has **Steam Guard** enabled, you'll be prompted for a code; when using **mobile confirmation**, Mirage prints a "still waiting" note in the log every 5 seconds while you confirm on your phone.
- If this machine already has a **validated SteamCMD session**, the wizard offers "Use saved session" so you don't have to re-enter your password.

The sign-in log updates in real time, so you can troubleshoot from it if something goes wrong. You can only reach the final step once sign-in succeeds. For details on privacy and credential handling, see [SteamCMD and Steam Sign-In](/en/workshop/steamcmd/).

## Step 4: Finish

Confirms setup is complete. Mirage remembers your username so it can reuse the session next time. From here you can [browse](/en/workshop/browse/) and [download](/en/workshop/download/) Workshop content as usual.

## Re-running the wizard

If you later need to sign in again or switch accounts, you can open the Workshop setup again. When a saved session expires, Mirage prompts you to sign in again with your password.

:::tip[Configure your API Key first]
The wizard mainly handles SteamCMD and sign-in. Browsing uses the Steam Web API, which ships with a built-in key shared by all users and can get busy. We recommend also adding your own [Steam Web API Key](/en/workshop/api-key/) for a more stable browsing experience.
:::
