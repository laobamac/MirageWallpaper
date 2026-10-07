---
title: 从源码构建
description: 安装依赖、构建三个渲染器和 Mirage 主程序，并在本地配置内置 Steam Web API Key。
---

Mirage 由三个独立渲染器（C++ / Objective-C++）、一个 SwiftUI 主程序和一个基于 SteamKit2 的 .NET Steam 服务组成。构建需要完整的 Xcode、.NET 10 SDK 和一组 Homebrew 依赖。

## 环境要求

- Intel Mac（`x86_64`）或 Apple Silicon Mac（`arm64`）
- macOS 14.2 或更高版本
- 完整版 Xcode（不是仅命令行工具）
- Homebrew
- CMake 4.3.1 或更高版本
- .NET 10 SDK

## 安装依赖

```bash
xcode-select --install
brew install cmake ninja pkg-config llvm molten-vk vulkan-loader vulkan-headers \
  glslang glfw freetype fontconfig lz4 ffmpeg dav1d nasm
```

渲染器构建脚本会自动构建固定版本的仅解码 FFmpeg；Homebrew FFmpeg 只用于生成测试媒体，不会打入应用。dav1d 用于 AV1 解码，nasm 用于 Intel 汇编优化。

这些依赖分别服务于：场景渲染器（Vulkan/MoltenVK、glslang、GLFW、FreeType、Fontconfig、LZ4）、视频渲染器（FFmpeg）以及 Homebrew LLVM 提供的 C++20 工具链。.NET 10 SDK 用于将 SteamKit2 服务分别发布为 `osx-arm64` 或 `osx-x64` 自包含程序。

## 构建

克隆仓库后，可直接使用根目录的一次性构建脚本：

```bash
git clone https://github.com/laobamac/MirageWallpaper.git
cd MirageWallpaper

./scripts/build_all.sh

open "Mirage/dist/Mirage.app"
```

最终 App 位于：

```text
Mirage/dist/Mirage.app
```

脚本会依次构建三个渲染器、Steam 服务和主程序，并把运行库、所需资源及 `MirageScreenSaver.saver` 一并嵌入 `Mirage.app`。SteamKit2 使用锁定依赖恢复，下载实现参考 DepotDownloader，但不需要安装 DepotDownloader 或任何外部下载器。

### 一次性构建全部

仓库根目录的 `scripts/build_all.sh` 会按依赖顺序编排全部构建：

```bash
./scripts/build_all.sh                 # 完整 release 构建：三个渲染器 + Steam 服务 + App
./scripts/build_all.sh debug           # debug 构建
./scripts/build_all.sh renderers       # 仅构建三个渲染器
./scripts/build_all.sh app             # 仅构建 App（假设渲染器已就绪）
./scripts/build_all.sh scene|web|video # 单独构建某个渲染器
./scripts/build_all.sh clean           # 清理所有子项目构建目录
```

可用环境变量：

| 变量 | 说明 |
| --- | --- |
| `JOBS=N` | 并行构建任务数（默认取逻辑核心数） |
| `MIRAGE_ARCH=arm64\|x86_64` | 主程序目标架构（默认当前主机架构） |
| `MIRAGE_STEAM_WEB_API_KEY` | 可选的内置 Steam Web API Key（32 位十六进制） |

### Debug 构建

执行 `./scripts/build_all.sh debug`。需要分开调试时，仍可直接运行各子项目的 `scripts/build.sh`。

## 本地配置内置 Steam Web API Key

源码不包含默认 API Key。本地完整打包时，可以把 Key 放入已被 Git 忽略的文件：

```bash
mkdir -p .secrets
chmod 700 .secrets
printf '%s\n' 'YOUR_32_CHARACTER_STEAM_WEB_API_KEY' > .secrets/steam_web_api_key
chmod 600 .secrets/steam_web_api_key
```

`Mirage/scripts/build.sh` 会读取该文件，通过临时 xcconfig 写入 App 的 Info.plist，并在构建结束后删除临时配置。也可以只对当前命令传入环境变量：

```bash
MIRAGE_STEAM_WEB_API_KEY='YOUR_32_CHARACTER_STEAM_WEB_API_KEY' \
  ./Mirage/scripts/build.sh Release
```

没有内置 Key 时 App 仍可正常编译，运行后可在[设置里填写自己的 Key](/workshop/api-key/)。

:::note[内置 Key 无法成为真正的秘密]
发布后的 App 必须包含该 Key，有能力分析 App 的人仍可提取。若需要不可提取的凭据，应把请求放到受控服务端，由服务端持有 Key，不要依赖客户端混淆。
:::
