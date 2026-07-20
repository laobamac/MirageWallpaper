# MirageQt

Linux Qt 6 implementation of Mirage Wallpaper.

The source tree mirrors the macOS app:

- `Sources/App` contains the Qt application entry point.
- `Sources/ContentView` contains the main installed-library and workshop UI.
- `Sources/Services` contains the Wallpaper Engine project parser, library scanner, settings, Steam Web API, SteamCMD and renderer process controller.
- `Sources/SettingsView` contains global settings.
- `Sources/SteamSetup` contains SteamCMD setup and login UI.
- `Sources/Renderers` contains Linux Qt web/video wallpaper renderers.

Dynamic desktop wallpapers are supported on X11 for scene wallpapers once the existing `SceneRenderer` `SceneWallpaper` binary has been built. Wayland sessions can run the main UI and preview content, but applying a live desktop wallpaper reports that the current session is unsupported.

`WebWallpaperQt` and `VideoWallpaperQt` are currently placeholder targets. They preserve the planned command-line shape and JSON stdin quit handling, while the main application reports that Linux web/video renderers are not implemented yet.

Build:

```sh
cmake -S MirageQt -B MirageQt/build -G Ninja
cmake --build MirageQt/build
```
