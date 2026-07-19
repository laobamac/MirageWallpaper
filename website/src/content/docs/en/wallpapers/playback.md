---
title: Playback control & custom properties
description: Adjust volume, speed, fill mode, and the custom properties bundled with each wallpaper.
---

Once you select a wallpaper, you can adjust its playback parameters in real time in the preview sidebar. These settings are saved per wallpaper and restored automatically the next time you apply the same wallpaper.

## Volume

Adjusts the playback volume of the current wallpaper. A volume of 0 is equivalent to muting. The final output is also affected by the [master volume in Settings](/en/settings/general/) and global mute.

## Speed

Adjusts the playback speed (`speed`). A speed of 0 means paused, and restoring a non-zero speed resumes playback.

## Fill mode

Controls how the wallpaper fits the screen:

| Mode | Behavior |
| --- | --- |
| Fill (cover) | Fills the screen, cropping the edges if needed |
| Fit (contain) | Shows the full content, possibly with letterboxing |
| Stretch | Stretches to fill, possibly changing the aspect ratio |

## Custom properties

Many Wallpaper Engine works define **user properties** (`general.properties`) in `project.json`, such as colors, toggles, sliders, dropdown options, and text content. Mirage reads these properties and generates the matching controls in the sidebar. Your changes are saved as `propertyOverrides` and pushed to the renderer process in real time.

The supported property types depend on the work's definition, with common ones being boolean toggles, slider values, colors, dropdown combo boxes, and text inputs. The exact meaning of each property is decided by the author. See [The project.json structure](/en/formats/project-json/) for details.

## Saving and restoring settings

Each wallpaper's runtime state (volume, speed, fill mode, property overrides) is persisted per wallpaper. Whether you switch wallpapers and switch back, or restart Mirage, your previous adjustments are restored.

## Relationship to global playback rules

The parameters above apply to a **single wallpaper**. On top of that, there's a layer of **global playback rules** that automatically take over playback behavior (continue / mute / pause / stop) in situations like a full-screen app being active, another app playing audio, the screen sleeping, or running on battery power. This is configured in [Performance & playback rules](/en/settings/performance/).
