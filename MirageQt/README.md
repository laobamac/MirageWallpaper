# MirageQt

Linux Qt 6 implementation of Mirage Wallpaper.

The source tree mirrors the macOS app:

- `Sources/App` contains the Qt application entry point.
- `Sources/ContentView` contains the main installed-library and workshop UI.
- `Sources/Services` contains the Wallpaper Engine project parser, library scanner, settings, Steam Web API, SteamCMD and renderer process controller.
- `Sources/SettingsView` contains global settings.
- `Sources/SteamSetup` contains SteamCMD setup and login UI.

Dynamic desktop wallpapers are supported on X11 for scene and video wallpapers once the sibling `SceneRenderer` and `VideoRenderer` desktop hosts have been built. MirageQt resolves `SceneWallpaper` and `VideoWallpaper` beside the application first, then from their repository build directories during development. Wayland sessions can run the main UI and preview content, but applying a live desktop wallpaper reports that the current session is unsupported.

The Linux web renderer is not implemented yet.

Build:

The mirage-display integration library is vendored in-tree under
`Vendors/MirageLinuxDisplay/` (pinned snapshot, see its README for the upstream
commit); no external `MirageLinuxDisplay` checkout is required.

```sh
cmake -S VideoRenderer -B VideoRenderer/build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build VideoRenderer/build/release

cmake -S MirageQt -B MirageQt/build -G Ninja
cmake --build MirageQt/build
```
