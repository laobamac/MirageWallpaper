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

<p align="center">
  <a href="#development-team">Development Team</a>
</p>

> [!IMPORTANT]
> **Mirage is still in an early stage.** If you encounter a problem, please file a detailed [GitHub Issue](https://github.com/laobamac/MirageWallpaper/issues/new/choose) with your macOS/App version, reproduction steps, expected and actual results, and relevant logs. You can also join the QQ feedback group: **2160040437**.

Mirage uses SwiftUI and AppKit for wallpaper browsing, management, and macOS integration. Three independent renderer processes play scene, web, and video wallpapers. It can discover local Wallpaper Engine-style wallpaper packages, browse the Steam Workshop, sign in to Steam, and download supported Workshop items.

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
- Includes an embedded Steam service based on [SteamKit2 3.4.0](https://github.com/SteamRE/SteamKit), with QR, password, and Steam Guard sign-in. Refresh tokens are stored in macOS Keychain.
- Calls SteamKit2 manifest and CDN APIs directly for Workshop downloads. The implementation follows proven design patterns from [DepotDownloader](https://github.com/SteamRE/DepotDownloader) without bundling or launching its executable.
- Reuses one persistent Steam session instead of starting and signing in again for every download.
- Downloads up to three Workshop items concurrently with live CDN byte counts, speed, progress, and estimated time remaining. Each task can be canceled independently.
- Plays downloaded works directly and exposes volume, playback rate, fill mode, position, and wallpaper-provided properties.
- Per-display playlists can rotate wallpapers by timer, logon, time of day, day of week, or video end, with sorted/random order and transitions.
- Supports multi-display coverage, menu-bar controls, login launch, the subscribed-items tab, and restoring a desktop placeholder image.
- Installs Mirage's standalone dynamic screen saver, which independently plays video and scene wallpapers while retaining the selected preset and custom properties.
- Offers two experimental dynamic lock-screen schemes: Scheme A requires macOS 26+, Scheme B requires macOS 14.2+; both support video and scene wallpapers only.
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
| Sign-in, Steam Guard, and downloading works | Embedded Steam service (SteamKit2; download flow informed by DepotDownloader) | No; a Steam account is required |

The built-in Steam Web API key is only used for initial browsing and is shared by all users. To avoid shared-rate-limit congestion, set your own key in **Settings → General → Steam API Key**. Obtain one from the [Steam API Key page](https://steamcommunity.com/dev/apikey).

Users in mainland China can choose the SteamCF browsing mirror in Settings. It only proxies Workshop browsing APIs; it does not accelerate Steam sign-in or content downloads and is only available in mainland China.

Mirage writes downloaded content to its own directory instead of reusing the system Steam client's data:

```text
~/Library/Application Support/Mirage/Workshop/content/431960
```

After sign-in, the Steam session remains active while Mirage runs. Mirage resolves items concurrently through the shared session and uses bounded CDN chunk concurrency to serve up to three items fairly. Canceling one task does not interrupt other downloads.

Mirage directly depends on SteamKit2 to communicate with Valve services. Its overall approach to manifest resolution, CDN server selection, chunk transfer, validation, and resumable reuse is informed by DepotDownloader. Mirage uses its own embedded service and task scheduler; it does not include the DepotDownloader command-line application or launch an external downloader.

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
- [.NET 10 SDK](https://dotnet.microsoft.com/download/dotnet/10.0)
- Homebrew LLVM, Ninja, pkg-config, MoltenVK, Vulkan Loader/Headers, glslang, GLFW, FreeType, Fontconfig, LZ4, and FFmpeg

Install dependencies:

```bash
xcode-select --install
brew install cmake ninja pkg-config llvm molten-vk vulkan-loader vulkan-headers \
  glslang glfw freetype fontconfig lz4 ffmpeg dav1d nasm
```

Renderer scripts automatically build a pinned decoder-only FFmpeg. Homebrew FFmpeg is used only to generate test media and is not bundled. dav1d supplies AV1 decoding; nasm supplies Intel assembly support.

## Build from Source

```bash
git clone https://github.com/laobamac/MirageWallpaper.git
cd MirageWallpaper

./scripts/build_all.sh

open "Mirage/dist/Mirage.app"
```

The resulting application is located at:

```text
Mirage/dist/Mirage.app
```

The app includes `MirageScreenSaver.saver`, which can be installed from **Settings → Screen Saver**. It is copied to `~/Library/Screen Savers` for the current user and does not require Mirage to remain running. The packaging script embeds the scene screen-saver runtime and required resources.

`build_all.sh` builds the three renderers, Steam service, and main app in order, then packages the complete app bundle. Use `./scripts/build_all.sh debug` for a Debug build or `./scripts/build_all.sh app` to rebuild only the app.

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
APPLE_DEVELOPER_ID_APPLICATION_P12             Base64-encoded Developer ID Application P12 certificate
APPLE_DEVELOPER_ID_APPLICATION_P12_PASSWORD    P12 certificate password
APPLE_NOTARY_APPLE_ID                          Apple ID
APPLE_NOTARY_PASSWORD                          Apple ID app-specific password
APPLE_DEVELOPER_TEAM_ID                        Apple Developer Team ID
```

If GitHub CLI is installed locally, you can run:

```bash
gh secret set MIRAGE_STEAM_WEB_API_KEY < .secrets/steam_web_api_key
```

`MIRAGE_SPARKLE_PRIVATE_KEY` is only used by Actions to generate Ed25519-signed updates and appcasts. Never commit it. Keep the original key in a logged-in keychain and maintain an offline backup. The client only contains the public key.

To compile the optional sign-in-free download component in the same Action, keep its source in a separate **private** repository and configure MirageWallpaper as follows:

| Type | Name | Value |
| --- | --- | --- |
| Repository variable | `MIRAGE_DIRECT_WORKSHOP_REPOSITORY` | The private component repository's `owner/name` |
| Repository variable | `MIRAGE_DIRECT_WORKSHOP_REF` | Its full 40-character commit SHA, shared by both architecture builds |
| Repository secret | `MIRAGE_DIRECT_WORKSHOP_DEPLOY_KEY` | An SSH deploy private key restricted to reading that repository; add its public key under the private repository's Deploy keys without write access |

The private repository must contain `Service`, `Tests/Tests.csproj`, `Tests/Program.cs`, `build.sh`, and `LICENSE`. Preserve the existing public `Service/VerificationKey.cs`; do not regenerate the signing identity. Do not commit `secrets/`, the activation signing private key, or test activation codes, or supply them to Actions. The workflow fetches the pinned commit into the runner's temporary directory, tests and compiles the helper, and bundles only its `dist` binaries. Private source and compiler logs are never uploaded as artifacts. Packaging verification checks that the helper starts and rejects an invalid activation code.

Leaving all three settings unset builds the standard edition. Partial configuration or a private build failure fails the job rather than silently omitting the feature. After updating the component, update `MIRAGE_DIRECT_WORKSHOP_REF` and run the Action manually or trigger the next main repository build. Changes to the private repository alone do not trigger the main workflow.

The workflow writes the full Git commit and an incrementing build number from `git rev-list --count` into the App. No manual version bump is required. Only a build with a greater build number is installed, preventing a newer development build from being downgraded to an older release.

- Pushing a `v*` tag creates a normal GitHub Release and stable update feed.
- Pushing to `main` replaces the rolling `prerelease` GitHub Release and beta update feed.
- The App checks and downloads stable updates automatically by default. Turning off automatic updates in **Settings → Software Update** stops background checking/downloading but keeps manual checks available. Enabling prerelease updates also checks the beta channel.
- Each architecture uses an independent appcast. Update archives, appcasts, and release notes are signed with Sparkle Ed25519.

On the next launch after updating, Mirage also checks any installed `MirageScreenSaver.saver`. It atomically replaces the installed component and restarts relevant screen-saver services only when its build number is older than the App's bundled component.

GitHub Secrets prevent a key from appearing in the repository and ordinary build logs, but they cannot make a key embedded in a distributed client truly secret. Anyone capable of inspecting the App can extract it. If a non-extractable credential is required later, move the request to a controlled server that holds the key; do not rely on client-side obfuscation.

The workflow imports the Developer ID Application certificate into a temporary keychain, signs the complete App with Hardened Runtime and a secure timestamp, submits it to Apple for notarization, staples the ticket, and verifies its signature, ticket, and Gatekeeper acceptance before packaging. Sparkle Ed25519 signing continues to protect the authenticity of subsequent updates independently.

## Data Directories

| Data | Default location |
| --- | --- |
| Mirage local wallpapers | `~/Library/Application Support/Mirage/Wallpapers` |
| Mirage Workshop downloads | `~/Library/Application Support/Mirage/Workshop/content/431960` |
| System Steam Workshop content | `~/Library/Application Support/Steam/steamapps/workshop/content/431960` |
| Workshop preview cache | `~/Library/Caches/Mirage/WorkshopCache` |
| Wallpaper runtime settings | `UserDefaults` |
| Dynamic screen-saver configuration | `~/Library/Application Support/Mirage/screensaver.json` |
| Installed dynamic screen saver | `~/Library/Screen Savers/MirageScreenSaver.saver` |

Mirage discovers valid works from the system Steam client, the Mirage download directory, and custom directories.

## Repository Layout

```text
.
├── Mirage/                 # SwiftUI / AppKit application and packaging scripts
│   └── Mirage Screen Saver/ # Standalone dynamic screen-saver target
├── SteamService/           # SteamKit2 authentication service and DepotDownloader-informed downloader
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
2. `./scripts/build_all.sh` produces a complete App bundle.
3. The App bundle contains all three renderers, runtime libraries, the MoltenVK ICD, and `assets`.
4. No API keys, Steam sign-in data, build directories, or user wallpapers are committed.

## Development Team

| Name | Role | GitHub |
| --- | --- | --- |
| Xiaoci Wang | Project Author · Developer | [@laobamac](https://github.com/laobamac) |
| Jiale Yu | Developer | [@dawalishi821](https://github.com/dawalishi821) |
| Pikachu Ren | Developer | [@PIKACHUIM](https://github.com/PIKACHUIM) |
| Yinan Qin | Developer | [@elysia-best](https://github.com/elysia-best) |

## Acknowledgements

- [SteamKit2](https://github.com/SteamRE/SteamKit) — direct dependency of Mirage's embedded Steam service; version 3.4.0, licensed under LGPL-2.1.
- [DepotDownloader](https://github.com/SteamRE/DepotDownloader) — an important design reference for Workshop manifest, CDN chunk download, and validation flows; licensed under GPL-2.0. Mirage does not bundle its executable.
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) for runtime translation.
- [wallpaper-engine-mac](https://github.com/MrWindDog/wallpaper-engine-mac) for the UI framework.
- [waywallen/ParticleSystem](https://github.com/waywallen/open-wallpaper-engine/blob/main/src/Scene/Particle/ParticleSystem.cpp) for the particle-system reference.
- [laobamac/OpenMetalWallpaper](https://github.com/laobamac/OpenMetalWallpaper) for model parsing.
- [rstd](https://github.com/hypengw/rstd).

## License

Mirage is released under [GPL-3.0](LICENSE). Steam service notices are stored in [`SteamService/Licenses`](SteamService/Licenses); all other third-party code and resources remain under their respective licenses. Mirage is not affiliated with or endorsed by Valve, Steam, or Wallpaper Engine.

Optional sign-in-free Workshop downloads are disabled by default and require a device-specific activation code in Settings → General. This mode supports downloads only, without Steam subscriptions, favorites or comments. Its independent helper is not included in this open-source repository; builds without it retain normal Steam downloads. Release builds can supply a precompiled component using `MIRAGE_DIRECT_WORKSHOP_BUNDLE`. Do not include private source or activation signing material in this repository.
