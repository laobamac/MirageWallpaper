# KDE Plasma 适配器

这是 Plasma 6 的壁纸包：Plasma 在 X11 与 Wayland 上同样拥有表面，插件
从不创建或放置 X11 窗口。

## 构建与安装

构建需要 CMake 3.20、Ninja、Qt 6.5 或更新版本（Gui、Qml、Quick）、
pkg-config 以及 EGL/GLESv2 开发包。`kpackagetool6` 用于安装壁纸包。
从仓库根目录执行：

```sh
cmake -S . -B build-kde-package -G Ninja \
  -DMIRAGE_DISPLAY_PLUGIN_QML=ON \
  -DMIRAGE_DISPLAY_WITH_EGL=ON \
  -DMIRAGE_DISPLAY_QML_URI=Mirage.DisplayEmbed
cmake --build build-kde-package --target mirage-wallpaper-package
```

构建目录只用于生成包，不会把核心库或 QML 模块安装到系统路径。

Qt Quick 显示项按场景图后端自动选择导入路径：

- **OpenGL/EGL**：`EGL_EXT_image_dma_buf_import` + 原生 fence 同步。
- **Vulkan**：external memory FD / DRM 修饰符导入；对无法直接采样的
  修饰符使用同设备 relay/blit 回退。

两种路径下，表面归属与输入都由 Plasma 负责。

适配器对 broker 启动竞态采用非阻塞连接：握手超过 5 秒会主动丢弃并重试，
场景图初始化失败也会退避重试；broker 重建 Unix socket 时通过目录监视和
设备/inode 变化立即建立新会话。因此启动期间显示 `disconnected` 不会把
Plasma 壁纸项永久留在失效会话上，broker 随后启动即可自动恢复。

壁纸配置中的 **Render backend** 提供 Automatic、OpenGL、Vulkan 三档选择。
Automatic 跟随 plasmashell 当前场景图；指定后端时，插件会严格校验实际
Qt Quick 后端，若不一致则停止连接并在诊断信息中提示错误。Qt Quick 图形
设备由 plasmashell 在启动时创建，切换后端需按对应方式配置并重启
plasmashell，壁纸插件不会在运行中切换设备。

**Vulkan 后端有一个前提：桌面程序必须放行 DMA-BUF。** 壁纸插件跑在
plasmashell 里，而 plasmashell 在启动时就把显卡功能清单定死了，壁纸
插件没法自己往里面加。所以走 Vulkan 后端时，如果桌面上报
`VK_ERROR_EXTENSION_NOT_PRESENT`（`memory FD properties query failed`），
需要先在驱动侧启用 NVIDIA 的 `nvidia-drm modeset=1`，再用
`QT_VULKAN_DEVICE_EXTENSIONS` 环境变量让 plasmashell 带上扩展清单，
完整步骤见[故障排查](/reference/troubleshooting/)。不想折腾的话，把
渲染后端切回 OpenGL 也能正常显示（EGL 路径不依赖这些扩展）。

窗口状态取自 `org.kde.taskmanager`（由 Plasma/KWin 工作区数据支撑，在
两种会话类型下行为一致），不查询 X11 窗口。`MirageSurfaceEmbed.qml`
变体用于把显示项嵌入其他 Plasma 界面（以 `MIRAGE_DISPLAY_QML_URI`
含 `Embed` 时启用打包）。

## 可安装壁纸包（kpackage）

构建时会在 `build-kde-package/adapters/kde/` 生成自包含的
`mirage-wallpaper-<版本>.zip`：这是标准
Plasma/Wallpaper kpackage，`metadata.json` 位于压缩包根目录，
`contents/ui/main.qml` 为主脚本，原生 QML 模块随包内置在
`contents/ui/MirageDisplayEmbed`，无需另行安装 QML 模块或系统库。安装到当前用户：

```sh
kpackagetool6 -t Plasma/Wallpaper \
  -i build-kde-package/adapters/kde/mirage-wallpaper-0.2.0.zip
```

卸载：

```sh
kpackagetool6 -t Plasma/Wallpaper -r org.mirage.wallpaper
```

说明：根目录 CPack 生成的 `mirage-linux-display-<版本>-Linux.zip` 是核心库
发行包，布局与壁纸包不同，不能直接交给 `kpackagetool6`。壁纸包由
`mirage-wallpaper-package` 目标生成。
