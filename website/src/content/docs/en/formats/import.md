---
title: Import Local Wallpapers
description: Import wallpapers into Mirage's local library from a folder or a video file.
---

Besides downloading from the Steam Workshop, you can also import local content directly into Mirage. Import accepts two kinds of input: **a wallpaper folder containing `project.json`**, and **video files**.

## Opening the Import Panel

There are a few ways to start an import:

- The import entry at the bottom of the wallpaper library in the main window.
- "Import Wallpaper…" (`I`) in the [menu bar](/en/wallpapers/menubar/).

The import panel lets you select multiple folders or video files at once. Supported inputs include folders as well as `.mp4`, `.mov`, and `.m4v` videos.

## Importing a Wallpaper Folder

When you select a folder, Mirage requires that its **root contains a `project.json`**. This is exactly the standard structure of a Wallpaper Engine wallpaper. Once the requirement is met, the entire folder is copied into the import directory.

If the selected folder has no `project.json`, the import fails with the message "The selected folder must contain project.json. Please check and try again." For the fields in this file, see [project.json Structure](/en/formats/project-json/).

## Importing a Video File

When you select an `.mp4`, `.mov`, or `.m4v` video, Mirage automatically wraps it into a video wallpaper package. This process does not re-encode the video; for the exact behavior, see [Converting Video to Wallpaper](/en/formats/video-convert/).

## Name Conflicts

If the target name already exists, Mirage automatically appends a number (such as `name_1`, `name_2`) to avoid overwriting an existing wallpaper.

## After Importing

Once the import finishes, Mirage refreshes the wallpaper library. Imported content is grouped under the "Import directory" source, which you can locate by source in the [filter panel](/en/wallpapers/library/). For the actual path, see [Data Directories](/en/advanced/data-directories/).

:::note
Importing **copies** the content rather than moving or referencing it, so the original files stay unchanged. As a result, importing large wallpapers takes up additional disk space.
:::
