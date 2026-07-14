<p align="center">
  <img src="Mirage/Mirage%20Wallpaper/Resources/Assets.xcassets/AppIcon.appiconset/icon_256.png" width="128" alt="Mirage 图标">
</p>

<h1 align="center">Mirage</h1>

<p align="center">
  面向 macOS 的原生动态壁纸管理器与 Wallpaper Engine 兼容运行时。
</p>

<p align="center">
  <a href="https://github.com/laobamac/MirageWallpaper/actions/workflows/build-macos.yml"><img alt="Build macOS App" src="https://github.com/laobamac/MirageWallpaper/actions/workflows/build-macos.yml/badge.svg"></a>
  <img alt="macOS" src="https://img.shields.io/badge/macOS-14.2%2B-000000?logo=apple&logoColor=white">
  <img alt="Architecture" src="https://img.shields.io/badge/architecture-x86__64%20%7C%20arm64-blue">
  <img alt="Swift" src="https://img.shields.io/badge/Swift-5-F05138?logo=swift&logoColor=white">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white">
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-GPL--3.0-blue"></a>
</p>

> [!IMPORTANT]
> **Mirage 当前仍处于早期阶段。** 如果遇到问题，请认真撰写 [GitHub Issue](https://github.com/laobamac/MirageWallpaper/issues/new/choose)，说明系统与 App 版本、复现步骤、预期结果、实际现象和相关日志；也可以加入 **QQ 交流群 2160040437** 反馈。

Mirage 使用 SwiftUI 与 AppKit 提供壁纸浏览、管理和系统集成，并通过三个独立渲染进程播放场景、网页和视频壁纸。应用可以读取本地 Wallpaper Engine 风格的壁纸包，也可以直接浏览 Steam 创意工坊、安装 SteamCMD、登录 Steam 并下载壁纸。

> Mirage 正在持续开发，Wallpaper Engine 场景格式的兼容性仍在完善。复杂作品可能存在特效、脚本或材质表现差异。

## 主要功能

- 支持 `scene`、`web`、`video` 三类动态壁纸。
- 浏览已安装壁纸，支持搜索、排序、类型/来源/标签/内容分级筛选和收藏。
- 直接导入包含 `project.json` 的目录，或把 `.mp4`、`.mov`、`.m4v` 视频转换为本地壁纸包。
- 浏览 Steam 创意工坊的趋势、最新、热门、评分和标签分类内容。
- 识别并下载创意工坊预设；缺少基础壁纸时会先征求同意再加入下载队列，依赖已安装时可直接应用。
- 由 Mirage 管理 SteamCMD 的安装、独立数据目录、Steam 登录和 Steam Guard 验证。
- 复用已登录的 SteamCMD 控制台会话，避免每次下载前重复启动和登录。
- 下载任务排队执行，展示启动、连接、下载、校验和完成状态；进度依据 SteamCMD 进程网络接收量与作品文件大小计算。
- 已下载作品可直接播放，并打开音量、速度、填充模式及作品自定义属性侧栏。
- 支持多显示器覆盖、菜单栏控制、登录启动和桌面占位图恢复。
- 可在全屏应用、其他应用播放音频、屏幕休眠或电池供电时选择继续、静音、暂停或停止。
- 网页壁纸首次运行前显示安全确认，并支持 Wallpaper Engine 用户属性与鼠标事件。

## 渲染架构

| 组件 | 技术 | 职责 |
| --- | --- | --- |
| Mirage | SwiftUI、AppKit | 界面、壁纸库、创意工坊、设置、进程管理和系统集成 |
| SceneWallpaper | C++20、Vulkan、MoltenVK | `scene.pkg` / `scene.json`、材质、粒子、LUT、文字和用户属性 |
| WebWallpaper | Objective-C++、WKWebView | HTML 壁纸、JavaScript、媒体、鼠标事件和用户属性 |
| VideoWallpaper | Objective-C++、AVFoundation | 视频循环、音量、速度和填充模式 |

渲染器作为独立进程运行。Mirage 通过标准输入发送逐行 JSON 控制消息，因此单个渲染器异常不会直接破坏主应用状态。

## Steam 创意工坊

Mirage 对 Steam 的两类访问彼此独立：

| 用途 | 服务 | 是否需要 API Key |
| --- | --- | --- |
| 浏览、搜索和读取作品信息 | Steam Web API | 是 |
| 登录、Steam Guard 和下载作品 | SteamCMD | 否，需要 Steam 账户 |

应用内置的 Steam Web API Key 只用于首次浏览，并由所有用户共享。建议在“设置 → 通用 → Steam API Key”中填写自己的 Key，以避免共享额度繁忙。Key 可在 [Steam Web API Key 申请页面](https://steamcommunity.com/dev/apikey) 获取。

中国大陆用户可以在设置中选择 SteamCF 浏览镜像。镜像只代理创意工坊浏览 API，不会加速 SteamCMD 登录或内容下载，并且仅允许中国大陆用户访问。

SteamCMD 使用 Mirage 自己的目录，不复用系统 Steam 客户端的数据：

```text
~/Library/Application Support/Mirage/steamcmd
```

登录成功后，SteamCMD 会话会在 Mirage 运行期间持续保持；退出 Mirage、主动退出登录、取消导致进程终止或会话失效时才会结束。下载队列当前串行执行，这是因为同一个交互式 SteamCMD 会话一次只能可靠处理一个创意工坊命令。

创意工坊预设会在浏览页、详情页、下载管理和“已安装”中明确标记。预设本身只保存属性与附带素材，并依赖一个基础壁纸：基础壁纸已经安装时，点击预设会直接应用并打开自定义侧栏；尚未安装时，Mirage 会显示基础壁纸名称和大小，询问是否一起下载。预设与基础壁纸都会保留为独立的已安装项目。

## 壁纸包格式

壁纸目录以 `project.json` 为入口：

```text
wallpaper-folder/
├── project.json
├── preview.jpg
└── wallpaper-file
```

最小视频壁纸示例：

```json
{
  "title": "My Wallpaper",
  "type": "video",
  "file": "demo.mp4",
  "preview": "preview.jpg"
}
```

| `type` | 常见入口 | 渲染方式 |
| --- | --- | --- |
| `scene` | `scene.pkg`、`scene.json` | SceneWallpaper |
| `web` | HTML 文件，通常为 `index.html` | WebWallpaper |
| `video` | 常见视频文件 | VideoWallpaper |

Mirage 会解析作品声明的入口文件，并对部分非标准目录布局进行兼容查找。目录仍必须包含有效的 `project.json`。

## 系统与构建要求

- Intel Mac（`x86_64`）或 Apple Silicon Mac（`arm64`）
- macOS 14.2 或更高版本
- 完整版 Xcode
- Homebrew
- CMake 4.3.1 或更高版本
- Homebrew LLVM、Ninja、pkg-config、MoltenVK、Vulkan Loader/Headers、glslang、GLFW、FreeType、Fontconfig、LZ4 和 FFmpeg

安装依赖：

```bash
xcode-select --install
brew install cmake ninja pkg-config llvm molten-vk vulkan-loader vulkan-headers \
  glslang glfw freetype fontconfig lz4 ffmpeg
```

## 从源码构建

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

Debug 构建只需把四条构建命令中的 `release` / `Release` 分别改成 `debug` / `Debug`。

### 本地配置内置 Steam Web API Key

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

没有内置 Key 时 App 仍可正常编译，开发者可以在运行后的设置中填写自己的 Key。

## GitHub Actions 自动打包

[Build macOS App](.github/workflows/build-macos.yml) 会在以下情况使用 `macos-15-intel` 和 `macos-15` 自动构建三个渲染器和 Mirage（x86_64 和 arm64）：

- 推送到 `main`；
- 推送名称以 `v` 开头的标签；
- 在 Actions 页面手动运行。

首次运行前，在仓库的 **Settings → Secrets and variables → Actions** 中添加 Repository Secret：

```text
Name:  MIRAGE_STEAM_WEB_API_KEY
Value: 32 位 Steam Web API Key
```

如果本机安装了 GitHub CLI，也可以执行：

```bash
gh secret set MIRAGE_STEAM_WEB_API_KEY < .secrets/steam_web_api_key
```

Workflow 会校验 Secret、构建并临时签名 App、验证三个渲染器、生成 `Mirage-<版本>-macOS-x86_64.zip` 和 `Mirage-<版本>-macOS-arm64.zip` 以及对应的 SHA-256 文件，再将它们保留为 30 天的 Actions Artifact。

每次推送到 `main` 且构建成功后，Workflow 还会用当前产物更新仓库唯一的 Pre-release。更新说明从最近一个正式 Release 开始收集到当前版本之间的全部 commit，每个 commit 单独一行；尚无正式 Release 时则列出完整提交历史。旧 Pre-release 及其标签会在发布前删除，正式 Release 不受影响。

GitHub Secrets 可以避免 Key 出现在仓库和普通构建日志中，但无法让客户端内置 Key 成为真正的秘密：发布后的 App 必须包含它，有能力分析 App 的人仍可以提取。若未来需要不可提取的凭据，应把对应请求放到受控服务端，由服务端持有 Key；不要依赖客户端混淆。

当前 Workflow 使用临时签名，不包含 Apple Developer ID 签名和公证，因此产物适合测试与内部分发。正式公开发布建议再配置 Developer ID、Hardened Runtime 和 Apple Notary Service。

## 数据目录

| 数据 | 默认位置 |
| --- | --- |
| Mirage 本地壁纸 | `~/Library/Application Support/Mirage/Wallpapers` |
| Mirage 管理的 SteamCMD | `~/Library/Application Support/Mirage/steamcmd` |
| Mirage SteamCMD 下载内容 | Mirage 管理目录中的 `steamapps/workshop/content/431960` |
| 系统 Steam 创意工坊内容 | `~/Library/Application Support/Steam/steamapps/workshop/content/431960` |
| 创意工坊预览缓存 | `~/Library/Caches/Mirage/WorkshopCache` |
| 壁纸运行时设置 | `UserDefaults` |

Mirage 会同时发现系统 Steam、Mirage SteamCMD 和自定义目录中的有效作品。

## 仓库结构

```text
.
├── Mirage/                 # SwiftUI / AppKit 主应用与打包脚本
├── SceneRenderer/          # C++20 + Vulkan/MoltenVK 场景渲染器
├── WebRenderer/            # WKWebView 网页渲染器
├── VideoRenderer/          # AVFoundation 视频渲染器
├── assets/                 # 场景运行时资源、材质、着色器、字体和 LUT
├── .github/workflows/      # macOS 自动构建与打包
└── LICENSE
```

## 独立调试渲染器

```bash
SceneRenderer/build/macos-clang-release/Tools/SceneViewer/SceneViewer <scene.pkg>
WebRenderer/build/release/Tools/WebViewer/WebViewer <web-wallpaper-directory>
VideoRenderer/build/release/Tools/VideoViewer/VideoViewer <video-wallpaper-directory>
```

桌面 Host 分别输出到各项目构建目录下的 `Tools/SceneWallpaper`、`Tools/WebWallpaper` 和 `Tools/VideoWallpaper`。

## 贡献

提交前请至少确认：

1. 三个渲染器可以独立构建；
2. `./Mirage/scripts/build.sh Release` 能生成完整 App Bundle；
3. App Bundle 中包含三个渲染器、运行时动态库、MoltenVK ICD 和 `assets`；
4. 没有提交 API Key、Steam 登录数据、构建目录或用户壁纸。

## 鸣谢

- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) 提供运行时转译
- [wallpaper-engine-mac](https://github.com/MrWindDog/wallpaper-engine-mac) 的UI框架 
- [waywallen/ParticleSystem](https://github.com/waywallen/open-wallpaper-engine/blob/main/src/Scene/Particle/ParticleSystem.cpp) 的粒子系统 
- [laobamac/OpenMetalWallpaper](https://github.com/laobamac/OpenMetalWallpaper) 的模型解析 
- [rstd](https://github.com/hypengw/rstd)

## 许可证

Mirage 使用 [GPL-3.0](LICENSE) 发布。仓库中的第三方代码与资源继续遵循各自许可证。Mirage 与 Valve、Steam 或 Wallpaper Engine 没有关联，也未获得其官方认可。
