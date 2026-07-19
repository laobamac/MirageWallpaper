---
title: Rendering Architecture
description: Mirage's multi-process rendering model - the main app, three independent renderers, and the communication between them.
---

Mirage uses a **multi-process architecture**: the SwiftUI main app handles the interface and scheduling, while the actual wallpaper rendering is delegated to three independent renderer processes. This way, a single wallpaper crash cannot bring down the whole app, and each tech stack can focus on what it does best.

## Component Breakdown

| Component | Language / Framework | Responsibility |
| --- | --- | --- |
| Mirage.app | Swift / SwiftUI | Main interface, wallpaper library, Workshop, settings, process scheduling |
| SceneWallpaper | C++20 / Vulkan (via MoltenVK → Metal) | Renders scene wallpapers |
| WebWallpaper | Objective-C++ / WKWebView | Renders web wallpapers |
| VideoWallpaper | Objective-C++ / AVFoundation | Renders video wallpapers |

The three renderers come from the `SceneRenderer`, `WebRenderer`, and `VideoRenderer` subprojects in the repository. Once built, they are invoked by the main app as executables.

## Process Scheduling

The RendererController in the main app starts, switches, and stops renderer processes:

- It picks the matching renderer binary based on the wallpaper type.
- **Each screen** can run its own renderer process, so different displays can show different wallpapers.
- Switching wallpapers follows a "candidate → ready → replace" flow: the new renderer prepares its first frame in the background, then replaces the old process that is currently on screen, avoiding a blank desktop during the switch.

## Inter-Process Communication

The main app sends commands to renderers over a **line-based JSON protocol on standard input**: each line is a single JSON instruction used to set the wallpaper, adjust properties, volume, speed, fill mode, frame rate, and so on. Renderers send status and logs back over standard output/standard error, which the main app parses line by line.

## Crash Isolation and Recovery

- Renderers are independent processes, so a crash only affects a single wallpaper; the main app and the other screens are unaffected.
- The main app listens for renderer exit events and can use them to clean up or restart according to policy.
- Policies in [Performance Settings](/MirageWallpaper/en/settings/performance/) such as "Stop (release memory)" simply terminate the corresponding renderer process to free up resources.

## Binary Lookup

The RendererController prefers the renderer binaries **bundled inside the app**. In a development environment, if the app doesn't contain them, it falls back to the `build/` outputs of each subproject (dev fallback), which makes iterating on the code easier.

- The scene renderer also depends on **MoltenVK** (the Vulkan → Metal translation layer) and its ICD configuration. It prefers the bundled copy and otherwise falls back to the Homebrew install location.

## Related

- To build these components, see [Building from Source](/MirageWallpaper/en/advanced/build/).
- To debug a single renderer on its own, see [Debugging Renderers](/MirageWallpaper/en/advanced/debug-renderers/).
- For a capability comparison across wallpaper types, see [Wallpaper Types](/MirageWallpaper/en/formats/wallpaper-types/).
