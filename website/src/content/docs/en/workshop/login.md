---
title: Steam Sign-In
description: Sign in to your Steam account in Mirage, handle Steam Guard, and reuse a saved session.
---

Downloading Workshop content requires signing in to a Steam account. Sign-in happens in step 3 of the [setup wizard](/MirageWallpaper/en/workshop/setup-wizard/), after which SteamCMD maintains the session.

## Sign in with a password

1. Enter your **Steam username and password**.
2. Mirage hands the credentials to the local SteamCMD process to complete sign-in. It **never uploads your password to any third-party server**, and doesn't keep it in plain text long-term.
3. The sign-in log updates in real time, so you can see the cause if something goes wrong.

Once sign-in succeeds, SteamCMD writes its own session config in the isolated directory. Mirage uses this to determine that you're signed in, and reuses it automatically for later downloads.

## Steam Guard

If your account has Steam Guard enabled, sign-in requires a second verification step:

- **Code method**: enter the email or token code to continue. If the session has ended, you'll be prompted to sign in again.
- **Mobile confirmation method**: Mirage enters a waiting state and prints "still waiting" in the log every 5 seconds. Tap confirm in the Steam mobile app, and it continues automatically.

## Reuse a saved session

If this machine already has a **validated SteamCMD session**, the wizard offers "Use saved session":

- You can continue without entering your password again.
- Mirage only persists your **username** for reuse; the session itself is maintained by SteamCMD.

If the saved session has become invalid, Mirage prompts: "The saved Steam session is no longer valid, please sign in again with your password."

## Cancel and retry

- You can **cancel** during sign-in at any time; Mirage clears the password and code inputs.
- Going back a step also safely cancels a sign-in in progress.
- If you run into problems, retry, or see [Troubleshooting](/MirageWallpaper/en/workshop/troubleshooting/).

## Privacy

- Your password is only used for local SteamCMD sign-in.
- Diagnostic logs redact sensitive fields like passwords, API keys, and tokens (replacing them with `[redacted]`).
- For more detail, see [SteamCMD and Steam Sign-In](/MirageWallpaper/en/workshop/steamcmd/).
