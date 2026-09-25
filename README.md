<p align="center">
  <img src="Mirage/Mirage%20Wallpaper/Resources/Assets.xcassets/AppIcon.appiconset/icon_256.png" width="128" alt="Mirage 图标">
</p>

<h1 align="center">Mirage</h1>

<p align="center">
  <a href="README_EN.md">English</a> · 简体中文
</p>

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

<p align="center">
  <a href="#开发团队">开发团队</a>
</p>

> [!IMPORTANT]
> **Mirage 当前仍处于早期阶段。** 如果遇到问题，请认真撰写 [GitHub Issue](https://github.com/laobamac/MirageWallpaper/issues/new/choose)，说明系统与 App 版本、复现步骤、预期结果、实际现象和相关日志；也可以加入 **QQ 交流群 2160040437** 反馈。

Mirage 使用 SwiftUI 与 AppKit 提供壁纸浏览、管理和系统集成，并通过三个独立渲染进程播放场景、网页和视频壁纸。应用可以读取本地 Wallpaper Engine 风格的壁纸包，也可以直接浏览 Steam 创意工坊、登录 Steam 并下载壁纸。

> Mirage 正在持续开发，Wallpaper Engine 场景格式的兼容性仍在完善。复杂作品可能存在特效、脚本或材质表现差异。

## 支持 Mirage

Mirage 会继续免费开放开发。如果它为你的桌面带来了价值，欢迎按自己的意愿赞助；每一份支持都会用于持续维护、兼容性改进和新功能开发。赞助完全自愿，不影响任何功能使用。

| 爱发电 | 微信支付 | 支付宝 |
| --- | --- | --- |
| <a href="https://www.ifdian.net/a/laobamac"><img src="Mirage/Mirage%20Wallpaper/Resources/Sponsorship/afdian.jpg" width="180" alt="在爱发电赞助 laobamac"></a><br>点击二维码或图片打开爱发电 | <img src="Mirage/Mirage%20Wallpaper/Resources/Sponsorship/wechat-pay.png" width="180" alt="微信支付赞助二维码"> | <img src="Mirage/Mirage%20Wallpaper/Resources/Sponsorship/alipay.jpg" width="180" alt="支付宝赞助二维码"> |

海外用户也可以赞助 USDT：

```text
0xFc0a5C52e3A085FEc7b077FE3D2C413114Bf880D
```

转账前请自行确认网络、地址和金额。

## 主要功能

- 支持 `scene`、`web`、`video` 三类动态壁纸。
- 浏览已安装壁纸，支持搜索、排序、类型/来源/标签/内容分级筛选和收藏。
- 直接导入包含 `project.json` 的目录，或把 `.mp4`、`.mov`、`.m4v` 视频转换为本地壁纸包。
- 浏览 Steam 创意工坊的趋势、最新、热门、评分和标签分类内容。
- 识别并下载创意工坊预设；缺少基础壁纸时会先征求同意再加入下载队列，依赖已安装时可直接应用。
- 内置基于 [SteamKit2 3.4.0](https://github.com/SteamRE/SteamKit) 的 Steam 服务，支持二维码、密码和 Steam Guard 登录，刷新令牌保存在 macOS 钥匙串。
- 创意工坊下载器直接调用 SteamKit2 的清单与 CDN API，实现参考 [DepotDownloader](https://github.com/SteamRE/DepotDownloader) 的成熟下载流程，但不捆绑或启动 DepotDownloader 可执行程序。
- 复用一个长驻 Steam 会话，避免每次下载前重复启动和登录。
- 最多同时下载三个创意工坊作品，实时显示 CDN 接收字节、下载速度、进度和预计剩余时间；每个任务可独立取消。
- 已下载作品可直接播放，并打开音量、速度、填充模式、画面位置及作品自定义属性侧栏。
- 支持按显示器保存播放列表，可按计时器、登录、当日时间、星期或视频结束自动切换，并提供有序/随机顺序和过渡效果。
- 支持多显示器覆盖、菜单栏控制、登录启动、已订阅作品页和桌面占位图恢复。
- 可安装 Mirage 自带的动态屏保，独立播放视频和场景壁纸，并保留当前预设与自定义属性。
- 设置中提供两套实验性动态锁屏方案：方案 A 需要 macOS 26+，方案 B 需要 macOS 14.2+；两者都只支持视频和场景壁纸。
- 可在全屏应用、其他应用播放音频、屏幕休眠或电池供电时选择继续、静音、暂停或停止。
- 使用 macOS“点按墙纸以显示桌面”时会自动恢复播放。
- 网页壁纸首次运行前显示安全确认，并支持 Wallpaper Engine 用户属性与鼠标事件。

## 渲染架构

| 组件 | 技术 | 职责 |
| --- | --- | --- |
| Mirage | SwiftUI、AppKit | 界面、壁纸库、创意工坊、设置、进程管理和系统集成 |
| SceneWallpaper | C++20、Vulkan、MoltenVK | `scene.pkg` / `scene.json`、材质、粒子、LUT、文字和用户属性 |
| WebWallpaper | Objective-C++、WKWebView | HTML 壁纸、JavaScript、媒体、鼠标事件和用户属性 |
| VideoWallpaper | Objective-C++、AVFoundation | 视频循环、音量、速度和填充模式 |
| MirageScreenSaver | Swift、WebKit、AVFoundation、Metal | 独立安装的动态屏保宿主 |

渲染器作为独立进程运行。Mirage 通过标准输入发送逐行 JSON 控制消息，因此单个渲染器异常不会直接破坏主应用状态。

## Steam 创意工坊

Mirage 对 Steam 的两类访问彼此独立：

| 用途 | 服务 | 是否需要 API Key |
| --- | --- | --- |
| 浏览、搜索和读取作品信息 | Steam Web API | 是 |
| 登录、Steam Guard 和下载作品 | 内置 Steam 服务（SteamKit2；下载流程参考 DepotDownloader） | 否，需要 Steam 账户 |

应用内置的 Steam Web API Key 只用于首次浏览，并由所有用户共享。建议在“设置 → 通用 → Steam API Key”中填写自己的 Key，以避免共享额度繁忙。Key 可在 [Steam Web API Key 申请页面](https://steamcommunity.com/dev/apikey) 获取。

中国大陆用户可以在设置中选择 SteamCF 浏览镜像。镜像只代理创意工坊浏览 API，不会加速 Steam 登录或内容下载，并且仅允许中国大陆用户访问。

Mirage 将下载内容写入自己的目录，不复用系统 Steam 客户端的数据：

```text
~/Library/Application Support/Mirage/Workshop/content/431960
```

登录成功后，Steam 会话会在 Mirage 运行期间持续保持。Mirage 使用共享会话并行解析作品，并通过受限的 CDN 分块并发公平地服务最多三个作品；取消一个任务不会中断其他下载。

Mirage 直接依赖 SteamKit2 与 Valve 服务通信。清单解析、CDN 服务器选择、分块下载、校验和断点复用的整体设计参考了 DepotDownloader；Mirage 使用自己的应用内服务和任务调度，不包含 DepotDownloader 命令行程序，也不会启动外部下载器。

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
- [.NET 10 SDK](https://dotnet.microsoft.com/download/dotnet/10.0)
- Homebrew LLVM、Ninja、pkg-config、MoltenVK、Vulkan Loader/Headers、glslang、GLFW、FreeType、Fontconfig、LZ4 和 FFmpeg

安装依赖：

```bash
xcode-select --install
brew install cmake ninja pkg-config llvm molten-vk vulkan-loader vulkan-headers \
  glslang glfw freetype fontconfig lz4 ffmpeg dav1d nasm
```

渲染器构建脚本会自动构建固定版本的仅解码 FFmpeg；Homebrew FFmpeg 只用于生成测试媒体，不会打入应用。dav1d 用于 AV1 解码，nasm 用于 Intel 汇编优化。

## 从源码构建

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

App 内包含可在“设置 → 屏保”中安装的 `MirageScreenSaver.saver`。屏保组件会被复制到当前用户的 `~/Library/Screen Savers`，不要求 Mirage 主程序保持运行。场景屏保运行库和所需资源由打包脚本一并嵌入。

`build_all.sh` 会按顺序构建三个渲染器、Steam 服务和主程序，并完成 App Bundle 打包。Debug 构建使用 `./scripts/build_all.sh debug`；只重建主程序时可使用 `./scripts/build_all.sh app`。

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
MIRAGE_STEAM_WEB_API_KEY      32 位 Steam Web API Key
MIRAGE_SPARKLE_PRIVATE_KEY    Mirage 专用 Sparkle Ed25519 私钥
APPLE_DEVELOPER_ID_APPLICATION_P12             Base64 编码的 Developer ID Application P12 证书
APPLE_DEVELOPER_ID_APPLICATION_P12_PASSWORD    P12 证书密码
APPLE_NOTARY_APPLE_ID                          Apple ID
APPLE_NOTARY_PASSWORD                          Apple ID 专用密码
APPLE_DEVELOPER_TEAM_ID                        Apple Developer Team ID
```

如果本机安装了 GitHub CLI，也可以执行：

```bash
gh secret set MIRAGE_STEAM_WEB_API_KEY < .secrets/steam_web_api_key
```

`MIRAGE_SPARKLE_PRIVATE_KEY` 只用于 Actions 生成 Ed25519 签名的更新包和 appcast。它绝不能提交到仓库；应保留登录钥匙串中的原始密钥，并另存一份离线备份。客户端仅包含可公开的公钥。

如需在同一次 Action 中编译免登录下载组件，将组件源码放在独立的 **Private** 仓库，再为 MirageWallpaper 配置：

| 类型 | 名称 | 内容 |
| --- | --- | --- |
| Repository Variable | `MIRAGE_DIRECT_WORKSHOP_REPOSITORY` | 私有组件仓库的 `owner/name` |
| Repository Variable | `MIRAGE_DIRECT_WORKSHOP_REF` | 私有组件的完整 40 位提交 SHA，两种架构使用同一版本 |
| Repository Secret | `MIRAGE_DIRECT_WORKSHOP_DEPLOY_KEY` | 仅用于读取上述私有仓库的 SSH 部署私钥；对应公钥加入私有仓库 Deploy keys，不启用写权限 |

私有仓库须包含 `Service`、`Tests/Tests.csproj`、`Tests/Program.cs`、`build.sh` 和 `LICENSE`；保留原来的 `Service/VerificationKey.cs` 公钥，不要重新生成签名身份。`secrets/`、签发私钥和测试激活码均不提交，也不配置到 Actions。工作流在 runner 临时目录读取指定提交，运行私有组件测试并编译，仅将 `dist` 二进制打入 App；私有源码和编译日志不会作为 artifact 上传。打包后会验证辅助进程可以运行，并拒绝无效激活码。

三项全部未配置时构建普通版本；配置不完整或私有组件构建失败时整个任务失败，避免悄悄发布缺少功能的版本。更新组件后修改 `MIRAGE_DIRECT_WORKSHOP_REF`，再手动运行 Action 或触发下一次主仓库构建。只更新私有仓库不会自动触发主仓库。

Workflow 为每次构建自动将完整 Git commit 与 `git rev-list --count` 生成的递增构建号写入 App，因此不需要手动更新版本号。只有构建号更高的 commit 才会被安装，避免把较新的开发构建降级为较旧 Release。

- 推送 `v*` 标签会创建正式 GitHub Release，并写入稳定更新源；
- 推送 `main` 会替换滚动的 `prerelease` GitHub Release，并写入 beta 更新源；
- App 默认自动检查并下载正式版；在“设置 → 软件更新”关闭自动更新后不再后台检查或下载，但仍可手动检查；开启“接收测试版更新”后，Sparkle 会同时检查 beta channel；
- 两个架构各自使用独立 appcast，更新包、appcast 和更新说明均经 Sparkle Ed25519 签名。

App 更新后的下一次启动会同时检查已经安装到 `~/Library/Screen Savers` 的 Mirage 屏保组件；仅当其构建号落后于 App 内置组件时才会原子替换并重启相关系统屏保服务。

GitHub Secrets 可以避免 Key 出现在仓库和普通构建日志中，但无法让客户端内置 Key 成为真正的秘密：发布后的 App 必须包含它，有能力分析 App 的人仍可以提取。若未来需要不可提取的凭据，应把对应请求放到受控服务端，由服务端持有 Key；不要依赖客户端混淆。

Workflow 会在临时钥匙串中导入 Developer ID Application 证书，以 Hardened Runtime 和安全时间戳签名完整 App，提交 Apple 公证、装订公证凭证，并在打包前通过签名、公证凭证和 Gatekeeper 校验。Sparkle Ed25519 签名继续独立保护后续更新的真实性。

## 数据目录

| 数据 | 默认位置 |
| --- | --- |
| Mirage 本地壁纸 | `~/Library/Application Support/Mirage/Wallpapers` |
| Mirage 创意工坊下载内容 | `~/Library/Application Support/Mirage/Workshop/content/431960` |
| 系统 Steam 创意工坊内容 | `~/Library/Application Support/Steam/steamapps/workshop/content/431960` |
| 创意工坊预览缓存 | `~/Library/Caches/Mirage/WorkshopCache` |
| 壁纸运行时设置 | `UserDefaults` |
| 动态屏保配置 | `~/Library/Application Support/Mirage/screensaver.json` |
| 已安装动态屏保 | `~/Library/Screen Savers/MirageScreenSaver.saver` |

Mirage 会同时发现系统 Steam、Mirage 下载目录和自定义目录中的有效作品。

## 仓库结构

```text
.
├── Mirage/                 # SwiftUI / AppKit 主应用与打包脚本
│   └── Mirage Screen Saver/ # 独立动态屏保目标
├── SteamService/           # SteamKit2 登录服务与参考 DepotDownloader 设计的下载器
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
2. `./scripts/build_all.sh` 能生成完整 App Bundle；
3. App Bundle 中包含三个渲染器、运行时动态库、MoltenVK ICD 和 `assets`；
4. 没有提交 API Key、Steam 登录数据、构建目录或用户壁纸。

## 开发团队

| 姓名 | 身份 | GitHub |
| --- | --- | --- |
| Xiaoci Wang | 项目作者 · 开发者 | [@laobamac](https://github.com/laobamac) |
| Jiale Yu | 开发者 | [@dawalishi821](https://github.com/dawalishi821) |
| Pikachu Ren | 开发者 | [@PIKACHUIM](https://github.com/PIKACHUIM) |
| Yinan Qin | 开发者 | [@elysia-best](https://github.com/elysia-best) |

## 鸣谢

- [SteamKit2](https://github.com/SteamRE/SteamKit) — Mirage 内置 Steam 服务的直接依赖，版本 3.4.0，使用 LGPL-2.1 许可证
- [DepotDownloader](https://github.com/SteamRE/DepotDownloader) — 创意工坊清单、CDN 分块下载与校验流程的重要设计参考，使用 GPL-2.0 许可证；Mirage 不捆绑其可执行程序
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) 提供运行时转译
- [wallpaper-engine-mac](https://github.com/MrWindDog/wallpaper-engine-mac) 的UI框架 
- [waywallen/ParticleSystem](https://github.com/waywallen/open-wallpaper-engine/blob/main/src/Scene/Particle/ParticleSystem.cpp) 的粒子系统 
- [laobamac/OpenMetalWallpaper](https://github.com/laobamac/OpenMetalWallpaper) 的模型解析 
- [rstd](https://github.com/hypengw/rstd)

## 许可证

Mirage 使用 [GPL-3.0](LICENSE) 发布。Steam 服务相关第三方声明位于 [`SteamService/Licenses`](SteamService/Licenses)，其余第三方代码与资源继续遵循各自许可证。Mirage 与 Valve、Steam 或 Wallpaper Engine 没有关联，也未获得其官方认可。

可选的免登录工坊下载默认关闭，需要在「设置 → 通用」输入本机专用激活码。该模式仅支持下载，不支持 Steam 订阅、收藏或评论。其独立辅助组件不包含在本开源仓库中；未提供该组件的构建仍可正常使用 Steam 下载。发布构建通过 `MIRAGE_DIRECT_WORKSHOP_BUNDLE` 指定预编译组件目录，勿将私有源码或激活签发材料加入仓库。
