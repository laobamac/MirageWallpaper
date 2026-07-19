---
title: Presets
description: Understand Workshop presets, their dependency on a base work, and how Mirage handles them automatically.
---

The Workshop has a special kind of item called a **preset**. A preset doesn't contain a complete wallpaper itself; instead it builds on another base work and overrides some of its custom properties to produce a different effect.

## What a preset consists of

In `project.json`, a preset is expressed through two fields:

- `dependency`: the Workshop ID of the base work it depends on.
- `preset`: a set of property values that override the base work's default properties.

Mirage uses these to recognize it as a preset and groups it under the "Preset" type in the filter panel.

## The dependency relationship

Because a preset only carries property overrides, not complete assets, **you must also have the base work it depends on for the preset to display correctly**. If you only download the preset and lack the dependency, the wallpaper won't render properly.

## How Mirage handles it automatically

When you try to apply a preset that's missing its dependency, Mirage will:

1. Detect that the dependency isn't installed yet.
2. Pop up a prompt explaining that the base work needs to be downloaded first.
3. After you confirm, automatically [download](/en/workshop/download/) the required dependency.
4. Once the dependency is ready, open and apply the preset.

If the dependency is already installed, Mirage applies the preset directly, with no extra steps.

## Tips

- When you see an item marked as a "Preset," check whether it needs an additional base work.
- Letting Mirage handle dependencies automatically is usually the easiest path; if a download is interrupted, re-trigger the apply to resume the flow.
- A preset's property overrides show up in the [Playback Controls and Custom Properties](/en/wallpapers/playback/) panel, where you can still fine-tune on top of them.

For more detail on properties and the `dependency` / `preset` fields, see [project.json Structure](/en/formats/project-json/).
