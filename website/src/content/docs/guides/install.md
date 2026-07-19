---
title: 安装与首次启动
description: 下载或构建 Mirage，完成首次启动，并处理 Gatekeeper 提示。
---

拿到 Mirage 有两条路径：下载现成的 Release，或从源码构建。

## 方式一：下载 Release（推荐）

1. 打开 [GitHub Releases](https://github.com/laobamac/MirageWallpaper/releases)。
2. 选择与你的 Mac 架构匹配的构建（`x86_64` 对应 Intel，`arm64` 对应 Apple Silicon）。
3. 下载后把 `Mirage.app` 拖入「应用程序」。

正式版（`v*` 标签）来自稳定更新源；`main` 分支的滚动构建会发布为 `prerelease`，走 beta 更新源。两个架构各自使用独立的 appcast，更新包和更新说明都经 Sparkle Ed25519 签名。

### 处理 Gatekeeper 提示

由于当前构建使用临时签名，首次打开时 macOS 可能提示无法验证开发者。此时可以在「系统设置 → 隐私与安全性」页面底部找到被拦截的 Mirage，点击「仍要打开」。后续版本更新的真实性由内置 Ed25519 公钥校验，不再依赖 Gatekeeper。

## 方式二：从源码构建

先确认满足[构建要求](/MirageWallpaper/guides/requirements/)，然后：

```bash
git clone https://github.com/laobamac/MirageWallpaper.git
cd MirageWallpaper

./SceneRenderer/scripts/build.sh release
./WebRenderer/scripts/build.sh release
./VideoRenderer/scripts/build.sh release
./Mirage/scripts/build.sh Release

open "Mirage/dist/Mirage.app"
```

最终 App 位于 `Mirage/dist/Mirage.app`。完整的构建说明、Debug 构建和内置 Steam Web API Key 的配置，见[从源码构建](/MirageWallpaper/advanced/build/)。

## 首次启动

首次启动后，建议按顺序完成几件事：

1. **了解界面**：顶部标签在「已安装」壁纸库和「发现 / 创意工坊」之间切换。参见[界面导览](/MirageWallpaper/guides/interface/)。
2. **添加壁纸**：可以直接[导入本地目录或视频](/MirageWallpaper/formats/import/)，也可以[设置 Steam 创意工坊](/MirageWallpaper/workshop/overview/)后下载。
3. **填写自己的 Steam Web API Key**（可选但推荐）：内置 Key 由所有用户共享，容易繁忙。见[Steam Web API Key](/MirageWallpaper/workshop/api-key/)。

:::tip
应用内包含可在「设置 → 屏保」中安装的 `MirageScreenSaver.saver`。安装后即使不保持 Mirage 运行，也能使用动态屏保。参见[动态屏保](/MirageWallpaper/screensaver/overview/)。
:::

## 数据存放在哪里

Mirage 的本地壁纸、SteamCMD 数据、缓存和配置都有固定位置，完整清单见[数据目录](/MirageWallpaper/advanced/data-directories/)。
