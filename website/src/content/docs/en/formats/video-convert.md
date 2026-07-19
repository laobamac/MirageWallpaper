---
title: Converting Video to Wallpaper
description: Wrap mp4, mov, and m4v videos into local video wallpaper packages that play right away.
---

Mirage can turn an ordinary video file into a live wallpaper. Just select a video during [import](/MirageWallpaper/en/formats/import/), and Mirage automatically generates a video wallpaper package.

## Supported Formats

- `.mp4`
- `.mov`
- `.m4v`

Other extensions are rejected with an unsupported-type message.

## What the Conversion Does

When you select a video, Mirage creates a new wallpaper package under the import directory and does the following:

1. **Copies the video file** into the newly created wallpaper folder. The conversion **does not re-encode** the video, so it is fast and lossless, but it also means playback compatibility depends on how the video itself is encoded.
2. **Generates a preview image**: it extracts a frame near the first second of the video and saves it as `preview.jpg` (JPEG, quality around 0.85).
3. **Writes `project.json`**: it generates a minimal project description, with `type` set to `video`, `file` pointing to the copied video, `preview` pointing to the thumbnail just generated, and `title` taken from the video file name.

The resulting wallpaper package looks roughly like this:

```text
My Video/
├── project.json      # type: "video", file, preview, title
├── My Video.mp4       # the copied original video
└── preview.jpg        # the auto-generated thumbnail
```

## Playback

The converted video wallpaper is rendered by VideoWallpaper (AVFoundation) and supports controls such as [volume, speed, and fill mode](/MirageWallpaper/en/wallpapers/playback/).

## Tips

- Since there is no re-encoding, prefer codecs that macOS / AVFoundation plays well (such as H.264 / HEVC in mp4 or mov). If a video plays fine in the system's Quick Look, it usually plays fine as a wallpaper too.
- To compress or transcode further, process the video with an external tool before importing (the project's build dependencies include FFmpeg, which is used mainly for renderer-side video processing; see [System and Build Requirements](/MirageWallpaper/en/guides/requirements/)).
- The conversion is a copy operation, so the original video is unaffected, but it takes up additional disk space.
