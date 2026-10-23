---
title: 构建与测试
description: 从源码构建 MirageLinuxDisplay 核心库、运行测试、构建 KDE 壁纸包与文档网站。
---

## 系统要求

- Linux，具备 C11 与 C++20 编译器（GCC 或 Clang）
- CMake 3.20 或更高版本、Ninja
- 可选：Vulkan 开发环境（`find_package(Vulkan)` 可检测）
- 可选：EGL / GLESv2 开发包（`pkg-config` 检测 `egl` 与 `glesv2`）
- 构建 KDE 壁纸包还需要 Qt 6.5+（Gui、Qml、Quick）与 `kpackagetool6`

## 核心库 + 测试

```sh
cmake -S . -B build -G Ninja -DMIRAGE_DISPLAY_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Vulkan 与 EGL 助手会在检测到相应开发环境时自动构建；也可以用 `-DMIRAGE_DISPLAY_WITH_VULKAN=OFF` / `-DMIRAGE_DISPLAY_WITH_EGL=OFF` 显式关闭。

## CMake 选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `MIRAGE_DISPLAY_WITH_VULKAN` | 检测到 Vulkan 时 ON | 构建 Vulkan DMA-BUF 助手 |
| `MIRAGE_DISPLAY_WITH_EGL` | 检测到 EGL/GLESv2 时 ON | 构建 EGL DMA-BUF 助手 |
| `MIRAGE_DISPLAY_BUILD_TESTS` | ON | 构建协议与会话单元测试 |
| `MIRAGE_DISPLAY_BUILD_EXAMPLES` | ON | 构建协议仿真示例 |
| `MIRAGE_DISPLAY_PLUGIN_QML` | OFF | 构建 Plasma Qt Quick 显示适配器 |
| `MIRAGE_DISPLAY_QML_URI` | — | QML 模块 URI，如 `Mirage.DisplayEmbed` |

## KDE Plasma 壁纸包

KDE Plasma 壁纸包由 `mirage-wallpaper-package` 目标单独构建。建议使用独立的构建目录，避免复用旧的根工程缓存；构建阶段只生成 ZIP，不会把核心库或 QML 模块写入系统路径：

```sh
cmake -S . -B build-kde-package -G Ninja \
  -DMIRAGE_DISPLAY_PLUGIN_QML=ON \
  -DMIRAGE_DISPLAY_WITH_EGL=ON \
  -DMIRAGE_DISPLAY_QML_URI=Mirage.DisplayEmbed
cmake --build build-kde-package --target mirage-wallpaper-package
```

生成 `build-kde-package/adapters/kde/mirage-wallpaper-0.2.0.zip` 后，用 KDE 包管理器安装到当前用户：

```sh
kpackagetool6 -t Plasma/Wallpaper \
  -i build-kde-package/adapters/kde/mirage-wallpaper-0.2.0.zip
```

:::caution[两种 ZIP 的区别]
根目录 CPack 生成的 `mirage-linux-display-<版本>-Linux.zip` 是核心库发行包，布局与壁纸包不同，不能直接交给 `kpackagetool6`。壁纸包只由 `mirage-wallpaper-package` 目标生成。
:::

## 示例

核心库构建后会产生两个示例：

- `mirage_headless_consumer`：无头消费端，打印缓冲池、配置与帧回调，用于协议调试。
- `mirage_mock_broker`：mock broker，模拟 `mirage-display-v1` 服务端。

详见[示例](/dev/examples/)。

## 文档网站

文档网站位于 `website/`，使用 Astro + Starlight，需要 Node.js 20+ 与 pnpm：

```sh
cd website
pnpm install
pnpm dev        # 本地预览 http://localhost:4321/MirageLinuxDisplay/
pnpm build      # 产出静态站点到 website/dist/
pnpm check      # astro check 类型与内容检查
```

`astro.config.mjs` 中的 `SITE` / `BASE` 默认指向 GitHub Pages 项目站点 `https://elysia-best.github.io/MirageLinuxDisplay/`，可以用环境变量覆盖。推送 `website/**` 到 `master` 时，[部署 workflow](/reference/troubleshooting/) 会自动构建并发布。
