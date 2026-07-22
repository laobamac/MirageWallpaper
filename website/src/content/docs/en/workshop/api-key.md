---
title: Steam Web API Key
description: Why and how to configure your own Steam Web API Key for more reliable Workshop browsing.
---

Mirage uses the **Steam Web API** to browse, search, and fetch details for Workshop items. This step needs an API Key.

## Built-in Key vs. custom Key

Mirage ships with a **built-in Steam Web API Key** that all users share. It works out of the box, but because everyone uses it, it can hit rate limits, return busy, or slow down during peak hours.

Once you add **your own API Key**, Mirage prefers it, making browsing and search more stable and faster. The Key must be a standard 32-character hexadecimal string; Mirage validates the format before enabling it, and falls back to the built-in Key if the format is invalid.

## How to get one

1. Sign in with your Steam account at the [Steam Web API Key registration page](https://steamcommunity.com/dev/apikey).
2. Fill in the domain as prompted (any domain you own or a placeholder works) and accept the terms.
3. Copy the generated 32-character Key.

## Entering it in Mirage

Open [Settings](/en/settings/general/), find the Steam Workshop options, and paste the Key in. Mirage trims leading and trailing whitespace, validates the format, and applies it immediately once valid.

## API endpoints

Mirage provides two endpoints:

- **Official**: `api.steampowered.com`
- **Mirror**: handy for network environments where the official endpoint is hard to reach

Switch endpoints in Settings. Whichever endpoint you use, it relies on the API Key you configured (or the built-in one).

:::note[Privacy]
Your API Key is stored in local settings. Mirage's diagnostic logs redact fields like keys, tokens, and passwords (replacing them with `[redacted]`), so they're never written to logs in plain text. Please don't share your Key publicly.
:::

## Common issues

- **Browsing keeps returning busy or times out**: usually the built-in Key is rate-limited; adding your own Key generally fixes it.
- **"Key is invalid"**: confirm it's a 32-character hexadecimal string with no extra spaces.
- **Slow access in some regions**: try switching to the mirror endpoint.

For more, see [Troubleshooting](/en/workshop/troubleshooting/).
