---
title: Software Update
description: Mirage's Sparkle-based automatic updates, beta channel, and update security.
---

Mirage uses the [Sparkle](https://sparkle-project.org/) framework for in-app updates. The relevant toggles live in the "Software Update" section of [General settings](/en/settings/general/).

## Automatic updates

- **Automatically check for and download updates**: When enabled, Mirage checks for and downloads available updates in the background.
- When disabled, Mirage no longer checks or downloads in the background, but you can still check manually anytime via "Check for Updates…" in the [menu bar](/en/wallpapers/menubar/).

Both automatic checking and automatic downloading follow the same toggle, and changes take effect immediately.

## Beta channel

- **Receive beta updates**: In addition to stable releases, also check for the latest **beta** builds (the beta channel, corresponding to rolling builds of the `main` branch).
- When enabled together with automatic updates, Mirage checks once right away.
- When disabled, you receive stable releases only.

Beta builds may include new features that aren't yet stable, best suited to users who like to try things early and can tolerate the occasional issue.

## Checking for updates manually

Whether or not automatic updates are enabled, you can check manually anytime:

- Menu bar icon → "Check for Updates…"

## Update security

- **Signature verification**: Update packages are verified by Sparkle using an **EdDSA (Ed25519) public key**. Only updates signed with the official private key are accepted, preventing tampering or replacement.
- **Per-architecture distribution**: Mirage provides separate update feeds (appcasts) for Apple Silicon (arm64) and Intel (x86_64), ensuring each device gets a build matching its own architecture.

## Recommendations

- Most users should keep automatic updates enabled to get fixes and new features promptly.
- Enable beta updates to experience new features first; stick with stable releases if you prioritize stability.

For version and acknowledgment information, see the "About" section of Settings.
