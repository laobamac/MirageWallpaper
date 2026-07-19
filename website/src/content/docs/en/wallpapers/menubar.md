---
title: Menu bar control
description: Control wallpaper playback directly from the system menu bar without opening the main window.
---

Mirage provides a status icon in the system menu bar. Click it to quickly control the current wallpaper without opening the main window.

## Menu items

The dropdown menu of the menu bar icon contains the following items:

| Menu item | Shortcut | Action |
| --- | --- | --- |
| Open Mirage | `O` | Show the main window |
| Import wallpaper… | `I` | Open the import panel to import a wallpaper from a folder |
| Settings | `,` | Open the settings window |
| Check for updates… | — | Manually check for updates via Sparkle |
| Project home page | — | Open the GitHub project page |
| Mute / Unmute | `M` | Toggle mute for the current wallpaper |
| Pause / Resume | `P` | Toggle between playing and paused |
| Cover all displays | — | Apply the current wallpaper to all screens |
| Stop wallpaper | — | Stop playback and clear the current wallpaper |
| Quit Mirage | `Q` | Quit the app |

## Dynamic menu items

**Mute** and **Pause** are dynamic items:

- After muting, the item becomes "Unmute" and the icon changes accordingly; unmuting returns it to "Mute".
- After pausing, the item becomes "Resume"; resuming playback changes it back to "Pause".

These two are linked with the volume and speed controls in the preview sidebar: no matter where you make the change, the menu text stays in sync.

## Icon and menu bar tint

Mirage uses its own menu bar icon. The "Adjust menu bar tint" option in Settings affects how the menu bar looks against a live wallpaper. See [General & audio settings](/MirageWallpaper/en/settings/general/) for details.
