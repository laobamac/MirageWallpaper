# MirageWallpaper — Plasma 壁纸包

这是 MirageLinuxDisplay 的 KDE Plasma 6 壁纸包（KPackage id：
`org.mirage.wallpaper`）。它通过 Plasma 自身的壁纸表面接收来自
MirageWallpaper 渲染器的 DMA-BUF 帧，并把指针与窗口状态回传给渲染器，
X11 与 Wayland 会话行为一致。

## 安装

本 ZIP 即标准 KPackage 布局（`metadata.json` 位于压缩包根目录），并且是
自包含的：原生 QML 模块随包内置在 `contents/ui/MirageDisplayEmbed`，因此
直接用 `kpackagetool6` 安装即可，无需另行安装 QML 模块或系统库：

```sh
kpackagetool6 -t Plasma/Wallpaper \
  -i build-kde-package/adapters/kde/mirage-wallpaper-0.2.0.zip
```

卸载：

```sh
kpackagetool6 -t Plasma/Wallpaper -r org.mirage.wallpaper
```

安装后在“桌面壁纸”中选择 **MirageWallpaper**，按需配置：

- **Display name**：输出名称（留空自动）。
- **Broker socket**：`mirage-display-v1` broker 的 Unix 域套接字路径
  （留空使用默认路径）。
- **Forward pointer events**：把指针事件回传给渲染器。
- **Show diagnostics**：显示后端、连接状态、输出与帧计数等信息。
- **Render backend**：选择 Automatic、OpenGL 或 Vulkan。Automatic 跟随
  plasmashell 当前场景图；指定后端时若实际 Qt Quick 后端不一致，插件会
  停止连接并在诊断信息中提示错误。切换后端需要调整 plasmashell 的启动
  环境并重启 plasmashell，插件不会运行时切换图形设备。

## 打包

在项目根目录（需要 Qt 6 与 EGL 开发环境）：

```sh
cmake -S . -B build-kde-package -G Ninja \
  -DMIRAGE_DISPLAY_PLUGIN_QML=ON \
  -DMIRAGE_DISPLAY_WITH_EGL=ON \
  -DMIRAGE_DISPLAY_QML_URI=Mirage.DisplayEmbed
cmake --build build-kde-package --target mirage-wallpaper-package
```

构建完成后在 `build-kde-package/adapters/kde/` 生成自包含的
`mirage-wallpaper-<版本>.zip`。

> 注意：根目录 CPack 生成的 `mirage-linux-display-<版本>-Linux.zip` 是
> 核心库发行包，不是壁纸包，不能用于 `kpackagetool6 -i`。
