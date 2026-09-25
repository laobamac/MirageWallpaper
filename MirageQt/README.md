# MirageQt

Linux Qt 6 implementation of Mirage Wallpaper.

The source tree mirrors the macOS app:

- `Sources/App` contains the Qt application entry point.
- `Sources/ContentView` contains the main installed-library and workshop UI.
- `Sources/Services` contains the Wallpaper Engine project parser, library scanner, settings, Steam Web API, SteamCMD and renderer process controller.
- `Sources/SettingsView` contains global settings.
- `Sources/SteamSetup` contains SteamCMD setup and login UI.

Dynamic desktop wallpapers are applied through the mirage-display protocol
(the repository-maintained `MirageLinuxDisplay` library): MirageQt hosts the display broker,
and the `SceneWallpaper` / `VideoWallpaper` / `WebWallpaper` renderer processes export frames to
the desktop environment's display adapter. The consumer adapter is the KDE
Plasma wallpaper plugin, so applying a live wallpaper requires a Plasma session
(either X11 or Wayland). MirageQt resolves `SceneWallpaper`, `VideoWallpaper`, and `WebWallpaper`
beside the application first, then from their repository build directories
during development.

Web wallpapers require the QtWebEngine runtime and the same Vulkan-capable
mirage-display/Plasma environment as scene wallpapers.

Build:

The mirage-display integration library is maintained in this repository at
`MirageLinuxDisplay/`; no external `MirageLinuxDisplay` checkout is required.

```sh
cmake -S VideoRenderer -B VideoRenderer/build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build VideoRenderer/build/release

cmake -S WebRenderer -B WebRenderer/build/linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DWEBRENDERER_BUILD_VIEWER=OFF
cmake --build WebRenderer/build/linux-release

cmake -S MirageQt -B MirageQt/build -G Ninja
cmake --build MirageQt/build
```

Native Linux packages are built with the repository packaging metadata:

```sh
dpkg-buildpackage -us -uc -b       # Debian 13 / Ubuntu 26.04, amd64
bash RPMBUILD/build.sh             # Fedora 44+, x86_64
```

Both package formats install `MirageQt` in `/usr/bin`, private renderer and
SteamService files in `/usr/libexec/miragewallpaper`, and shared wallpaper
assets in `/usr/share/miragewallpaper`. The desktop session still needs a
compatible mirage-display adapter to consume renderer frames.

多显示器播放状态按 `kde:` 稳定输出 ID 持久化，而不是按 Qt 当前屏幕索引
持久化。这样 KDE 调整屏幕顺序或主屏后，壁纸仍回到原输出；旧版按索引保存
的 `playlists.json` 会在首次启动迁移，并保留一份
`playlists.json.pre-stable-id` 备份。
