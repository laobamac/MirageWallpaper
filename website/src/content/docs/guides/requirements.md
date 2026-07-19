---
title: 系统与构建要求
description: 运行与从源码构建 Mirage 所需的系统版本、硬件架构和依赖。
---

Mirage 是一款原生 macOS 应用，同时支持 Intel 和 Apple Silicon。

## 运行要求

- **Intel Mac（`x86_64`）或 Apple Silicon Mac（`arm64`）**
- **macOS 14.2 或更高版本**

从 GitHub Releases 下载的构建包含两个架构所需的全部渲染器、运行时动态库、MoltenVK ICD 和 `assets` 资源，直接运行即可。

:::note[关于签名]
当前自动构建使用临时签名，不包含 Apple Developer ID 签名和公证。首次安装的用户可能需要在 macOS Gatekeeper 中手动允许 Mirage。后续更新的真实性由内置的 Sparkle Ed25519 公钥验证。
:::

## 构建要求

如果你打算[从源码构建](/MirageWallpaper/advanced/build/)，还需要以下工具链：

- 完整版 **Xcode**
- **Homebrew**
- **CMake 4.3.1 或更高版本**
- Homebrew 提供的 **LLVM、Ninja、pkg-config、MoltenVK、Vulkan Loader / Headers、glslang、GLFW、FreeType、Fontconfig、LZ4 和 FFmpeg**

一次性安装依赖：

```bash
xcode-select --install
brew install cmake ninja pkg-config llvm molten-vk vulkan-loader vulkan-headers \
  glslang glfw freetype fontconfig lz4 ffmpeg
```

各依赖的用途：

| 依赖 | 用途 |
| --- | --- |
| LLVM | 编译 C++20 场景渲染器 |
| Ninja、CMake | 渲染器构建系统 |
| MoltenVK、Vulkan Loader / Headers | 场景渲染器在 macOS 上通过 Vulkan → Metal 转译运行 |
| glslang | 编译着色器 |
| GLFW | 渲染器调试工具的窗口 |
| FreeType、Fontconfig | 场景文字渲染 |
| LZ4 | 场景资源解压 |
| FFmpeg | 视频处理 |

确认环境就绪后，前往[安装与首次启动](/MirageWallpaper/guides/install/)。
