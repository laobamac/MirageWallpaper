---
title: Building from Source
description: Install dependencies, build the three renderers and the Mirage main app, and configure a built-in Steam Web API Key locally.
---

Mirage consists of three independent renderers (C++ / Objective-C++), a SwiftUI main app, and a .NET Steam service based on SteamKit2. Building requires a full Xcode installation, the .NET 10 SDK, and a set of Homebrew dependencies.

## Requirements

- An Intel Mac (`x86_64`) or an Apple Silicon Mac (`arm64`)
- macOS 14.2 or later
- Full Xcode (not just the command line tools)
- Homebrew
- CMake 4.3.1 or later
- .NET 10 SDK

## Installing Dependencies

```bash
xcode-select --install
brew install cmake ninja pkg-config llvm molten-vk vulkan-loader vulkan-headers \
  glslang glfw freetype fontconfig lz4 ffmpeg dav1d nasm
```

Renderer scripts automatically build a pinned decoder-only FFmpeg. Homebrew FFmpeg is used only to generate test media and is not bundled. dav1d supplies AV1 decoding; nasm supplies Intel assembly support.

These dependencies serve, respectively: the scene renderer (Vulkan/MoltenVK, glslang, GLFW, FreeType, Fontconfig, LZ4), the video renderer (FFmpeg), and the C++20 toolchain provided by Homebrew LLVM. The .NET 10 SDK publishes the SteamKit2 service as a self-contained `osx-arm64` or `osx-x64` executable.

## Building

After cloning the repository, use the one-shot build script from the repository root:

```bash
git clone https://github.com/laobamac/MirageWallpaper.git
cd MirageWallpaper

./scripts/build_all.sh

open "Mirage/dist/Mirage.app"
```

The final app is located at:

```text
Mirage/dist/Mirage.app
```

The script builds the three renderers, Steam service, and main app in order, then embeds the runtime libraries, required resources, and `MirageScreenSaver.saver` into `Mirage.app`. SteamKit2 is restored from the lock file. DepotDownloader informed the downloader design but is not installed or invoked.

### Building Everything at Once

`scripts/build_all.sh` in the repository root orchestrates the entire build in dependency order:

```bash
./scripts/build_all.sh                 # full release build: three renderers + Steam service + app
./scripts/build_all.sh debug           # debug build
./scripts/build_all.sh renderers       # build the three renderers only
./scripts/build_all.sh app             # build the app only (assumes renderers are ready)
./scripts/build_all.sh scene|web|video # build a single renderer
./scripts/build_all.sh clean           # clean all subproject build directories
```

Available environment variables:

| Variable | Description |
| --- | --- |
| `JOBS=N` | Number of parallel build jobs (defaults to the logical core count) |
| `MIRAGE_ARCH=arm64\|x86_64` | Target architecture for the main app (defaults to the host architecture) |
| `MIRAGE_STEAM_WEB_API_KEY` | Optional built-in Steam Web API Key (32-digit hex) |

### Debug Builds

Run `./scripts/build_all.sh debug`. Individual subproject `scripts/build.sh` commands remain available when debugging one component in isolation.

## Configuring a Built-in Steam Web API Key Locally

The source code does not include a default API Key. For a full local build, you can place the key in a file that is already ignored by Git:

```bash
mkdir -p .secrets
chmod 700 .secrets
printf '%s\n' 'YOUR_32_CHARACTER_STEAM_WEB_API_KEY' > .secrets/steam_web_api_key
chmod 600 .secrets/steam_web_api_key
```

`Mirage/scripts/build.sh` reads this file, writes it into the app's Info.plist through a temporary xcconfig, and deletes the temporary config after the build finishes. You can also pass an environment variable for a single command only:

```bash
MIRAGE_STEAM_WEB_API_KEY='YOUR_32_CHARACTER_STEAM_WEB_API_KEY' \
  ./Mirage/scripts/build.sh Release
```

The app still compiles fine without a built-in key; after running it, you can [enter your own key in the settings](/en/workshop/api-key/).

:::note[A built-in key can never be a real secret]
A released app must contain the key, so anyone able to analyze the app can still extract it. If you need a credential that cannot be extracted, put the request on a controlled server that holds the key, rather than relying on client-side obfuscation.
:::
