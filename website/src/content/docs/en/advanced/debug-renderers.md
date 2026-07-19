---
title: Debugging Renderers Standalone
description: Run SceneWallpaper, WebWallpaper, and VideoWallpaper on their own from the command line and debug them by sending JSON control messages over standard input.
---

Mirage's three renderers are all independent executables. The main app drives them by forking child processes and feeding line-based JSON over standard input. This design lets a renderer run on its own, separate from the main app, which makes it easier to pin down "whether the problem is in the renderer itself or in the main app's scheduling."

:::note[This page is for developers]
If you're just using Mirage, you don't need this page. It's meant for people who want to diagnose rendering issues or build on the source code.
:::

## Where the Renderer Binaries Are

The main app looks up renderers in the order "prefer the app's bundled copy, and fall back to the source build directory if not found":

| Type | Source build output path (dev fallback) |
| --- | --- |
| Scene | `SceneRenderer/build/<preset>/Tools/SceneWallpaper/SceneWallpaper` |
| Web | `WebRenderer/build/release/Tools/WebWallpaper/WebWallpaper` |
| Video | `VideoRenderer/build/release/Tools/VideoWallpaper/VideoWallpaper` |

A properly packaged app copies them inside `Mirage.app`; during development, if there is no bundled copy, the main app derives the repository root from the source file's compile-time path and falls back to the build directories above. This means **as long as you've built the renderers in the repository, running the main app directly from Xcode loads the latest renderers** without repackaging each time.

## Control Protocol

Renderers receive **line-based JSON**: each line is a complete JSON object terminated by a newline `\n`. The main app writes these commands into the child process's standard input; renderers also send log and status lines back over standard output/standard error.

Because the channel is standard input, you can drive a renderer manually in a terminal — just feed it the JSON commands one line at a time.

:::caution[Protocol fields follow the source]
The specific fields of a control message (key names, value ranges) evolve across versions. This page only explains the unchanging part — "how to connect to a renderer"; **for the exact fields of each command, follow the parsing logic in the corresponding renderer's source**, and don't copy any fixed example verbatim, to avoid mismatches with the current version.
:::

## Running a Renderer Manually

Using the video renderer as an example, build it first and then run it directly:

```bash
# Build (release)
./VideoRenderer/scripts/build.sh release

# Run directly and send JSON control messages line by line from the terminal
./VideoRenderer/build/release/Tools/VideoWallpaper/VideoWallpaper
```

Once the process starts, it waits for line-based JSON on standard input. You can:

- Type JSON commands one line at a time and watch the renderer's output and window behavior; or
- Use a script to feed a sequence of commands in line by line:

```bash
printf '%s\n' '<first JSON command>' '<second JSON command>' \
  | ./VideoRenderer/build/release/Tools/VideoWallpaper/VideoWallpaper
```

The scene renderer (`SceneWallpaper`) also depends on the runtime resource directory `assets/` (shaders, materials, fonts, etc.). The main app prefers the app's bundled `assets` and falls back to the `assets` at the repository root. When running the scene renderer manually, make sure it can find this resource directory.

:::danger[Do not modify the contents of `assets/`]
The `assets/` directory contains resources and test wallpapers required by the scene runtime library. When debugging, only read from it — do not modify any files, so you don't break the renderer's runtime environment.
:::

## Watching How the Main App Drives Renderers

If you want to see which commands the main app actually sends, rather than constructing them yourself, you can:

1. Raise the log level in "Settings → About / Developer" (see [Settings Overview](/MirageWallpaper/en/settings/overview/)).
2. Run the main app from Xcode and watch the console for logs like `[Mirage] 启动渲染器: …` to confirm which binary is loaded and which screen it acts on.
3. The renderer's own standard output/standard error is forwarded by the main app and appears in the same log stream.

## Why Split into Separate Processes

- **Crash isolation**: when a single renderer exits abnormally, it doesn't directly bring down the main app's state; the main app can restart it per policy or fall back to a placeholder image.
- **Per-screen independence**: with multiple displays, each screen can run its own renderer instance, without interfering with the others.
- **Easier debugging**: you can reproduce a problem outside the main app, decoupling the "rendering" and "scheduling" layers.

For the overall process model, see [Rendering Architecture](/MirageWallpaper/en/advanced/architecture/).
