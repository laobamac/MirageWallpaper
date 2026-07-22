<!--
  MirageWallpaper
  Copyright © 2026 王孝慈. All rights reserved.
-->

<p align="center">
  <img src="Mirage/Mirage%20Wallpaper/Resources/Assets.xcassets/AppIcon.appiconset/icon_256.png" width="128" alt="Mirage icon">
</p>

<h1 align="center">Mirage</h1>

<p align="center">
  English · <a href="README.md">简体中文</a>
</p>

<p align="center">
  A native macOS live-wallpaper manager and a compatible runtime for Wallpaper Engine content.
</p>

<p align="center">
  <a href="https://github.com/laobamac/MirageWallpaper/actions/workflows/build-macos.yml"><img alt="Build macOS App" src="https://github.com/laobamac/MirageWallpaper/actions/workflows/build-macos.yml/badge.svg"></a>
  <img alt="macOS" src="https://img.shields.io/badge/macOS-14.2%2B-000000?logo=apple&logoColor=white">
  <img alt="Architecture" src="https://img.shields.io/badge/architecture-x86__64%20%7C%20arm64-blue">
  <img alt="Swift" src="https://img.shields.io/badge/Swift-5-F05138?logo=swift&logoColor=white">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white">
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-GPL--3.0-blue"></a>
</p>

> [!IMPORTANT]
> **Mirage is still in an early stage.** If you encounter a problem, please file a detailed [GitHub Issue](https://github.com/laobamac/MirageWallpaper/issues/new/choose) with your macOS/App version, reproduction steps, expected and actual results, and relevant logs. You can also join the QQ feedback group: **2160040437**.

Mirage uses SwiftUI and AppKit for wallpaper browsing, management, and macOS integration. Three independent renderer processes play scene, web, and video wallpapers. It can discover local Wallpaper Engine-style wallpaper packages, browse the Steam Workshop, install SteamCMD, sign in to Steam, and download supported Workshop items.

> Mirage is actively developed. Compatibility with Wallpaper Engine scene content is still improving, and complex works may differ in effects, scripts, or materials.

## Support Mirage

Mirage will remain free and open for development. If it brings value to your desktop, voluntary support is warmly appreciated; every contribution helps fund maintenance, compatibility improvements, and new features. Sponsorship is entirely optional and never affects functionality.

| Afdian | WeChat Pay | Alipay |
| --- | --- | --- |
| <a href="https://www.ifdian.net/a/laobamac"><img src="Mirage/Mirage%20Wallpaper/Resources/Sponsorship/afdian.jpg" width="180" alt="Sponsor laobamac on Afdian"></a><br>Click the image to open Afdian | <img src="Mirage/Mirage%20Wallpaper/Resources/Sponsorship/wechat-pay.png" width="180" alt="WeChat Pay sponsorship QR code"> | <img src="Mirage/Mirage%20Wallpaper/Resources/Sponsorship/alipay.jpg" width="180" alt="Alipay sponsorship QR code"> |

International users may also support via USDT:

```text
0xFc0a5C52e3A085FEc7b077FE3D2C413114Bf880D
```

Please independently verify the network, address, and amount before transferring.

## Highlights

- Supports `scene`, `web`, and `video` live wallpapers.
- Browses installed wallpapers with search, sorting, type/source/tag/content-rating filters, and favorites.
- Imports directories containing `project.json`, or turns `.mp4`, `.mov`, and `.m4v` files into local wallpaper packages.
- Browses Workshop trending, recent, popular, top-rated, and tag-based content.
- Detects and downloads Workshop presets; asks before downloading a required base wallpaper.
- Manages SteamCMD installation, an isolated data directory, Steam sign-in, and Steam Guard verification.
- Reuses an authenticated interactive SteamCMD session instead of repeatedly starting and signing in before every download.
- Queues downloads and reports launch, connection, download, validation, and completion states. Progress is based on SteamCMD network input and the Workshop item's file size.
- Plays downloaded works directly and exposes volume, playback rate, fill mode, and wallpaper-provided properties.
- Supports multi-display coverage, menu-bar controls, login launch, and restoring a desktop placeholder image.
- Installs Mirage's standalone dynamic screen saver, which can play video, web, and scene wallpapers while retaining the selected preset and custom properties.
- Lets you continue, mute, pause, or stop playback when another app is fullscreen, another app plays audio, the display sleeps, or the Mac is on battery.
- Restores playback after macOS "click wallpaper to reveal desktop" interaction.
- Shows a security confirmation before first running a web wallpaper and supports Wallpaper Engine user properties and mouse events.

## Rendering Architecture

| Component | Technology | Responsibility |
| --- | --- | --- |
| Mirage | SwiftUI, AppKit | UI, wallpaper library, Workshop, settings, process management, and macOS integration |
| SceneWallpaper | C++20, Vulkan, MoltenVK | `scene.pkg` / `scene.json`, materials, particles, LUTs, text, and user properties |
| WebWallpaper | Objective-C++, WKWebView | HTML wallpapers, JavaScript, media, mouse events, and user properties |
| VideoWallpaper | Objective-C++, AVFoundation | Video looping, volume, playback rate, and fill mode |
| MirageScreenSaver | Swift, WebKit, AVFoundation, Metal | Independently installed dynamic screen-saver host |

Renderers run as separate processes. Mirage sends line-delimited JSON control messages through standard input, so a renderer failure does not directly corrupt the main application.

## Steam Workshop

Mirage uses two independent Steam integrations:

| Purpose | Service | API key required? |
| --- | --- | --- |
| Browsing, search, and Workshop metadata | Steam Web API | Yes |
| Sign-in, Steam Guard, and downloading works | SteamCMD | No; a Steam account is required |

The built-in Steam Web API key is only used for initial browsing and is shared by all users. To avoid shared-rate-limit congestion, set your own key in **Settings → General → Steam API Key**. Obtain one from the [Steam API Key page](https://steamcommunity.com/dev/apikey).

Users in mainland China can choose the SteamCF browsing mirror in Settings. It only proxies Workshop browsing APIs; it does not accelerate SteamCMD sign-in or downloads and is only available in mainland China.

SteamCMD uses its own Mirage-managed directory rather than the system Steam client's data:

```text
~/Library/Application Support/Mirage/steamcmd
```

After sign-in, the SteamCMD console session remains alive while Mirage runs. It ends only when Mirage quits, the user signs out, cancellation terminates the process, or the session expires. Downloads are currently serialized because one interactive SteamCMD session can reliably handle only one Workshop command at a time.

Workshop presets are explicitly marked in browsing, details, download management, and installed views. A preset contains property values and optional assets, but depends on a base wallpaper. If the dependency is installed, selecting the preset applies it immediately and opens customization. Otherwise, Mirage displays the dependency's name and size and asks whether to download it as well. Presets and base wallpapers remain separate installed items.

## Wallpaper Package Format

A wallpaper directory uses `project.json` as its entry point:

```text
wallpaper-folder/
├── project.json
├── preview.jpg
└── wallpaper-file
```

Minimal video-wallpaper example:

```json
{
  "title": "My Wallpaper",
  "type": "video",
  "file": "demo.mp4",
  "preview": "preview.jpg"
}
```

| `type` | Typical entry | Renderer |
| --- | --- | --- |
| `scene` | `scene.pkg`, `scene.json` | SceneWallpaper |
| `web` | An HTML file, usually `index.html` | WebWallpaper |
| `video` | A common video file | VideoWallpaper |

Mirage resolves the declared entry point and supports some non-standard directory layouts for compatibility. A valid `project.json` is still required.

## System and Build Requirements

- Intel (`x86_64`) or Apple Silicon (`arm64`) Mac
- macOS 14.2 or later
- Full Xcode installation
- Homebrew
- CMake 4.3.1 or later
- Homebrew LLVM, Ninja, pkg-config, MoltenVK, Vulkan Loader/Headers, glslang, GLFW, FreeType, Fontconfig, LZ4, and FFmpeg

Install dependencies:

```bash
xcode-select --install
brew install cmake ninja pkg-config llvm molten-vk vulkan-loader vulkan-headers \
  glslang glfw freetype fontconfig lz4 ffmpeg
```

## Build from Source

```bash
git clone https://github.com/laobamac/MirageWallpaper.git
cd MirageWallpaper

./SceneRenderer/scripts/build.sh release
./WebRenderer/scripts/build.sh release
./VideoRenderer/scripts/build.sh release
./Mirage/scripts/build.sh Release

open "Mirage/dist/Mirage.app"
```

The resulting application is located at:

```text
Mirage/dist/Mirage.app
```

The app includes `MirageScreenSaver.saver`, which can be installed from **Settings → Screen Saver**. It is copied to `~/Library/Screen Savers` for the current user and does not require Mirage to remain running. The packaging script embeds the scene screen-saver runtime and required resources.

For a Debug build, replace `release` / `Release` in the four build commands with `debug` / `Debug`.

### Configure a Built-in Steam Web API Key Locally

The source tree does not contain a default API key. For a complete local package, place your key in a Git-ignored file:

```bash
mkdir -p .secrets
chmod 700 .secrets
printf '%s\n' 'YOUR_32_CHARACTER_STEAM_WEB_API_KEY' > .secrets/steam_web_api_key
chmod 600 .secrets/steam_web_api_key
```

`Mirage/scripts/build.sh` reads this file, writes it through a temporary xcconfig into the App's `Info.plist`, and removes the temporary configuration when the build ends. You can instead provide the environment variable for one command:

```bash
MIRAGE_STEAM_WEB_API_KEY='YOUR_32_CHARACTER_STEAM_WEB_API_KEY' \
  ./Mirage/scripts/build.sh Release
```

Without a built-in key, the App still builds normally; set a key in Settings after launching it.

## GitHub Actions Packaging

[Build macOS App](.github/workflows/build-macos.yml) uses `macos-15-intel` and `macos-15` to build all three renderers and Mirage for x86_64 and arm64 when:

- changes are pushed to `main`;
- a tag beginning with `v` is pushed; or
- the workflow is run manually from the Actions page.

Before the first run, add these Repository Secrets in **Settings → Secrets and variables → Actions**:

```text
MIRAGE_STEAM_WEB_API_KEY      32-character Steam Web API key
MIRAGE_SPARKLE_PRIVATE_KEY    Mirage's Sparkle Ed25519 private key
```

If GitHub CLI is installed locally, you can run:

```bash
gh secret set MIRAGE_STEAM_WEB_API_KEY < .secrets/steam_web_api_key
```

`MIRAGE_SPARKLE_PRIVATE_KEY` is only used by Actions to generate Ed25519-signed updates and appcasts. Never commit it. Keep the original key in a logged-in keychain and maintain an offline backup. The client only contains the public key.

The workflow writes the full Git commit and an incrementing build number from `git rev-list --count` into the App. No manual version bump is required. Only a build with a greater build number is installed, preventing a newer development build from being downgraded to an older release.

- Pushing a `v*` tag creates a normal GitHub Release and stable update feed.
- Pushing to `main` replaces the rolling `prerelease` GitHub Release and beta update feed.
- The App checks and downloads stable updates automatically by default. Turning off automatic updates in **Settings → Software Update** stops background checking/downloading but keeps manual checks available. Enabling prerelease updates also checks the beta channel.
- Each architecture uses an independent appcast. Update archives, appcasts, and release notes are signed with Sparkle Ed25519.

On the next launch after updating, Mirage also checks any installed `MirageScreenSaver.saver`. It atomically replaces the installed component and restarts relevant screen-saver services only when its build number is older than the App's bundled component.

GitHub Secrets prevent a key from appearing in the repository and ordinary build logs, but they cannot make a key embedded in a distributed client truly secret. Anyone capable of inspecting the App can extract it. If a non-extractable credential is required later, move the request to a controlled server that holds the key; do not rely on client-side obfuscation.

The current workflow uses temporary signing and does not include Apple Developer ID signing or notarization. First-time users may need to manually allow Mirage in macOS Gatekeeper, while subsequent update authenticity is verified with the built-in Ed25519 public key.

## Data Directories

| Data | Default location |
| --- | --- |
| Mirage local wallpapers | `~/Library/Application Support/Mirage/Wallpapers` |
| Mirage-managed SteamCMD | `~/Library/Application Support/Mirage/steamcmd` |
| Mirage legacy SteamCMD content | `~/Library/Application Support/Mirage/steamcmd/steamapps/workshop/content/431960` |
| Mirage current SteamCMD download content | `~/Library/Application Support/Mirage/steamcmd/home/Library/Application Support/Steam/steamapps/workshop/content/431960` |
| System Steam Workshop content | `~/Library/Application Support/Steam/steamapps/workshop/content/431960` |
| Workshop preview cache | `~/Library/Caches/Mirage/WorkshopCache` |
| Wallpaper runtime settings | `UserDefaults` |
| Dynamic screen-saver configuration | `~/Library/Application Support/Mirage/screensaver.json` |
| Installed dynamic screen saver | `~/Library/Screen Savers/MirageScreenSaver.saver` |

Mirage discovers valid works from the system Steam client, Mirage SteamCMD, and custom directories.

## Repository Layout

```text
.
├── Mirage/                 # SwiftUI / AppKit application and packaging scripts
│   └── Mirage Screen Saver/ # Standalone dynamic screen-saver target
├── SceneRenderer/          # C++20 + Vulkan/MoltenVK scene renderer
├── WebRenderer/            # WKWebView web renderer
├── VideoRenderer/          # AVFoundation video renderer
├── assets/                 # Scene runtime assets, materials, shaders, fonts, and LUTs
├── .github/workflows/      # macOS build and packaging automation
└── LICENSE
```

## Run Renderers Independently

```bash
SceneRenderer/build/macos-clang-release/Tools/SceneViewer/SceneViewer <scene.pkg>
WebRenderer/build/release/Tools/WebViewer/WebViewer <web-wallpaper-directory>
VideoRenderer/build/release/Tools/VideoViewer/VideoViewer <video-wallpaper-directory>
```

Desktop hosts are produced under each renderer's build directory: `Tools/SceneWallpaper`, `Tools/WebWallpaper`, and `Tools/VideoWallpaper`.

## Contributing

Before submitting a change, verify at least that:

1. All three renderers build independently.
2. `./Mirage/scripts/build.sh Release` produces a complete App bundle.
3. The App bundle contains all three renderers, runtime libraries, the MoltenVK ICD, and `assets`.
4. No API keys, Steam sign-in data, build directories, or user wallpapers are committed.

## Acknowledgements

- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) for runtime translation.
- [wallpaper-engine-mac](https://github.com/MrWindDog/wallpaper-engine-mac) for the UI framework.
- [waywallen/ParticleSystem](https://github.com/waywallen/open-wallpaper-engine/blob/main/src/Scene/Particle/ParticleSystem.cpp) for the particle-system reference.
- [laobamac/OpenMetalWallpaper](https://github.com/laobamac/OpenMetalWallpaper) for model parsing.
