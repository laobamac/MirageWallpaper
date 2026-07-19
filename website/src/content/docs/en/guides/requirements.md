---
title: System & build requirements
description: The system version, hardware architecture, and dependencies needed to run and build Mirage from source.
---

Mirage is a native macOS app that supports both Intel and Apple Silicon.

## Runtime requirements

- **Intel Mac (`x86_64`) or Apple Silicon Mac (`arm64`)**
- **macOS 14.2 or later**

Builds downloaded from GitHub Releases include all the renderers, runtime dynamic libraries, MoltenVK ICD, and `assets` resources needed for both architectures, so they run as-is.

:::note[About signing]
Current automated builds use ad-hoc signing and don't include an Apple Developer ID signature or notarization. First-time users may need to allow Mirage manually in macOS Gatekeeper. The authenticity of later updates is verified by the built-in Sparkle Ed25519 public key.
:::

## Build requirements

If you plan to [build from source](/MirageWallpaper/en/advanced/build/), you'll also need the following toolchain:

- Full **Xcode**
- **Homebrew**
- **CMake 4.3.1 or later**
- From Homebrew: **LLVM, Ninja, pkg-config, MoltenVK, Vulkan Loader / Headers, glslang, GLFW, FreeType, Fontconfig, LZ4, and FFmpeg**

Install the dependencies in one go:

```bash
xcode-select --install
brew install cmake ninja pkg-config llvm molten-vk vulkan-loader vulkan-headers \
  glslang glfw freetype fontconfig lz4 ffmpeg
```

What each dependency is for:

| Dependency | Purpose |
| --- | --- |
| LLVM | Compiling the C++20 scene renderer |
| Ninja, CMake | Renderer build system |
| MoltenVK, Vulkan Loader / Headers | Running the scene renderer on macOS via Vulkan → Metal translation |
| glslang | Compiling shaders |
| GLFW | Windowing for the renderer debug tools |
| FreeType, Fontconfig | Scene text rendering |
| LZ4 | Scene asset decompression |
| FFmpeg | Video processing |

Once your environment is ready, head to [Install & first launch](/MirageWallpaper/en/guides/install/).
