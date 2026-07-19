---
title: 从源码构建
description: 安装依赖、构建三个渲染器和 Mirage 主程序，并在本地配置内置 Steam Web API Key。
---

Mirage 由三个独立渲染器（C++ / Objective-C++）和一个 SwiftUI 主程序组成。构建需要完整的 Xcode 和一组 Homebrew 依赖。

## 环境要求

- Intel Mac（`x86_64`）或 Apple Silicon Mac（`arm64`）
- macOS 14.2 或更高版本
- 完整版 Xcode（不是仅命令行工具）
- Homebrew
- CMake 4.3.1 或更高版本

## 安装依赖

```bash
xcode-select --install
brew install cmake ninja pkg-config llvm molten-vk vulkan-loader vulkan-headers \
  glslang glfw freetype fontconfig lz4 ffmpeg
```

这些依赖分别服务于：场景渲染器（Vulkan/MoltenVK、glslang、GLFW、FreeType、Fontconfig、LZ4）、视频渲染器（FFmpeg）以及 Homebrew LLVM 提供的 C++20 工具链。

## 构建

克隆仓库后，按依赖顺序构建三个渲染器，再构建主程序：

```bash
git clone https://github.com/laobamac/MirageWallpaper.git
cd MirageWallpaper

./SceneRenderer/scripts/build.sh release
./WebRenderer/scripts/build.sh release
./VideoRenderer/scripts/build.sh release
./Mirage/scripts/build.sh Release

open "Mirage/dist/Mirage.app"
```

最终 App 位于：

```text
Mirage/dist/Mirage.app
```

主程序的构建脚本会把三个渲染器和场景运行库、所需资源一并嵌入到 `Mirage.app`，其中也包含可在设置中安装的 `MirageScreenSaver.saver`。

### 一次性构建全部

仓库根目录的 `scripts/build_all.sh` 会按依赖顺序编排全部构建：

```bash
scripts/build_all.sh              # 完整 release 构建：三个渲染器 + App
scripts/build_all.sh debug        # debug 构建
scripts/build_all.sh renderers    # 仅构建三个渲染器
scripts/build_all.sh app          # 仅构建 App（假设渲染器已就绪）
scripts/build_all.sh scene|web|video   # 单独构建某个渲染器
scripts/build_all.sh clean        # 清理所有子项目构建目录
```

可用环境变量：

| 变量 | 说明 |
| --- | --- |
| `JOBS=N` | 并行构建任务数（默认取逻辑核心数） |
| `MIRAGE_ARCH=arm64\|x86_64` | 主程序目标架构（默认当前主机架构） |
| `MIRAGE_STEAM_WEB_API_KEY` | 可选的内置 Steam Web API Key（32 位十六进制） |

### Debug 构建

把四条命令中的 `release` / `Release` 分别改成 `debug` / `Debug` 即可。

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

没有内置 Key 时 App 仍可正常编译，运行后可在[设置里填写自己的 Key](/MirageWallpaper/workshop/api-key/)。

:::note[内置 Key 无法成为真正的秘密]
发布后的 App 必须包含该 Key，有能力分析 App 的人仍可提取。若需要不可提取的凭据，应把请求放到受控服务端，由服务端持有 Key，不要依赖客户端混淆。
:::
