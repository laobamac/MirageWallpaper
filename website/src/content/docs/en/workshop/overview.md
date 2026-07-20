---
title: Workshop Overview
description: Learn how Mirage integrates with the Steam Workshop and what you need to set up before using it.
---

Mirage has built-in browsing and downloading for the Steam Workshop, mapped directly to Wallpaper Engine's Workshop content (App ID `431960`). You can browse a huge library of wallpapers inside the app, with downloads handled by the SteamCMD instance Mirage manages for you.

![Browsing the Steam Workshop with the Nature filter in Mirage](/images/docs/workshop-nature.webp)

*The real Workshop interface after sign-in, filtered to the Nature tag.*

## How it works

The Workshop feature relies on two components working together:

- **Steam Web API**: used to browse, search, and fetch item details. Mirage uses it to pull trending, newest, most-subscribed, top-rated, and tag-categorized content. See [Steam Web API Key](/en/workshop/api-key/).
- **SteamCMD**: Valve's official command-line tool, used to actually download Workshop items. Mirage manages its installation, its dedicated data directory, and Steam sign-in. See [SteamCMD and Steam Sign-In](/en/workshop/steamcmd/).

## Before you start

The first time you switch to the Workshop tab, Mirage walks you through a four-step setup wizard:

1. **Welcome and overview**
2. **Install / detect SteamCMD**
3. **Sign in to Steam** (Steam Guard supported, saved sessions reusable)
4. **Finish**

For the details, see [Workshop Setup Wizard](/en/workshop/setup-wizard/).

## Browsing and downloading

Once setup is done, you can:

- Browse sorted by **Trending / Newest / Most Subscribed / Top Rated**.
- Filter by tag (Anime, Nature, Cyberpunk, Game, and more).
- Search by keyword.
- View item details, then download to your local wallpaper library.

See [Browse the Workshop](/en/workshop/browse/) and [Download and Manage](/en/workshop/download/).

## API endpoints

Mirage offers two Steam Web API endpoints: **official** (`api.steampowered.com`) and a **mirror**. If the official endpoint is unreliable on your network, you can switch to the mirror endpoint in [Settings](/en/settings/general/).

:::caution[Respect the terms]
Workshop content belongs to its respective authors. Please follow the license and terms of use set by Steam and the authors. Mirage is not affiliated with, nor endorsed by, Valve, Steam, or Wallpaper Engine.
:::
