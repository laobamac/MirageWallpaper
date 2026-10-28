# Mirage Editor

<img src="assets/MirageEditor.png" alt="Mirage Editor" width="128" height="128">

面向 macOS 的独立壁纸编辑器，基于 Wallpaper Engine 编辑器组件与 Wine、DXVK、MoltenVK 运行环境，提供壁纸工程编辑、实时 GPU 预览和 macOS 界面集成。

**Mirage Editor 为闭源专有软件，独立于 Mirage Wallpaper 主项目的开源授权管理。使用本编辑器必须合法持有 Wallpaper Engine 授权，并遵守相关商业许可条款。**

本仓库公开自有适配代码及工具，仍按专有条款提供。Wallpaper Engine 的程序、界面代码、资源、原始备份及包含其代码的界面补丁清单均不随仓库分发。

## 功能

- 打开、编辑和保存 Wallpaper Engine 壁纸工程。
- 编辑场景图层、材质、效果、粒子及工程属性。
- 通过 GPU 实时预览壁纸与交互效果。
- macOS 风格的窗口控制与界面，支持系统浅色、深色外观。
- 原生工程选择对话框与关于窗口，界面语言跟随系统设置。
- 独立启动编辑器，无需启动 Mirage Wallpaper 主程序。

## 系统要求

- macOS，当前构建面向 Intel Mac（x86_64）。
- 支持 Metal 的图形设备。
- 完整的应用包及配套 `runtime/` 目录。
- 通过合法渠道取得的 [Wallpaper Engine](https://store.steampowered.com/app/431960/Wallpaper_Engine/) 使用授权。

## 快速开始

### 从 Mirage 打开

Mirage 主窗口左下角的「编辑器」会复用现有 Steam 登录。首次使用在主窗口的浮层中验证当前账号的 Wallpaper Engine 永久自有许可证，并显示下载大小、预计安装占用和安装期间所需空间。点击「下载并安装」后从 Steam 下载原版组件，显示进度、当前文件、速度及预计剩余时间，完成本地组装后打开编辑器。

编辑器启动成功后，Mirage 主窗口与启动浮层一起淡出，并收起 Mirage 的 Dock 图标。编辑器显示在前台，Mirage 可通过菜单栏入口重新打开。

「设置 → 通用 → 管理编辑器」及入口的右键菜单提供修复、缓存清理和卸载。关闭浮层不停止下载；取消后可再次继续。缓存清理保留编辑器偏好，卸载保留用户工程。运行中的编辑器须先保存并退出，再进行修复或卸载。

安装数据位于 `~/Library/Application Support/Mirage/Editor/`：`active/` 存放当前安装，`downloads/` 存放可复用的 Steam 下载，`Projects/` 与 `UserData/` 分别保存工程和界面偏好。Steam 凭据只由 Mirage 的 Steam 服务管理，不传入 Wine 或编辑器网页。

### 独立开发环境

1. 将完整的 `StandaloneEditor` 文件夹放置在本地可读写目录。
2. 双击 `Mirage Editor.app`。
3. 在「文件 → 打开」中选择壁纸工程文件夹或其中的 `project.json`。
4. 编辑工程，通过「文件 → 保存」保存修改。

工程可以存放在 `Projects/` 或其他本地目录。打开现有工程时，保存操作会写入该工程所在目录。

移动安装位置时，请整体移动 `StandaloneEditor` 文件夹，保留应用包与 `runtime/` 的相对位置。

## 目录结构

```text
StandaloneEditor/
├── Mirage Editor.app       macOS 应用入口
├── Projects/               本地壁纸工程
├── assets/                 编辑器图标与生成记录
├── src/                    自有适配层源码与配置
├── tools/                  内部构建、启动和校验工具
├── runtime/                编辑器组件与运行环境
│   ├── engines/            Wine 引擎
│   ├── originals/          版本校验与重建所需的原始组件
│   ├── private/            本地专有界面补丁清单
│   ├── prefix-devel/       独立 Wine 环境与注册表
│   ├── windows/            编辑器程序与资源
│   └── logs/               运行日志
└── README.md
```

## 兼容性

适配层与以下组件版本绑定：

| 组件 | 版本 |
| --- | --- |
| Wallpaper Engine 编辑器 | 2.8.42 |
| Wine | 11.18 · x86_64 |
| DXVK | 1.10.3 |

运行环境通过 DXVK 与 MoltenVK 将 Direct3D 绘制转换为 Metal，并将预览纹理合成至编辑器界面。不同工程使用的效果、脚本和资源可能存在兼容性差异。

请勿单独替换上述组件。构建工具会校验原始文件的 SHA-256，拒绝处理不匹配的版本。

## 预览与系统音频

预览适配层复用 GPU 程序和共享纹理，避免重复尺寸通知；画布不可见或销毁后停止重绘，并支持图形上下文恢复。显示画布不再额外申请深度、模板和多重采样缓冲，场景自身的分辨率、抗锯齿和效果设置不变。CEF 的 Metal 图层使用异步呈现，保留场景的正常提交与帧率控制。

Metal 编译适配会识别可证明只读的双声道 64 项频谱数组，直接读取对应的常量缓冲，保留动态索引与边界检查。不匹配的 shader 使用原始路径；优化后的编译失败时也会回退。优化版本写入 Metal 源码以区分编译缓存，无需清理工程或 WE assets 中的 shader 缓存。

系统音频响应使用 macOS 原生 Core Audio taps，要求 macOS 14.2 或更新版本及系统音频采集权限。首次需要采集时由 macOS 管理授权；未获授权时无法取得系统音频。采集在进程私有设备中完成，经 Wine 原有音频链路交给编辑器，不生成录音文件，也不更改系统默认音频设备。

壁纸的静音、音量和播放配置继续生效。例如，声音组件启用「在编辑器中静音」时，普通编辑模式保持静音，运行预览按工程配置播放。

## 命令行使用

以下命令在 `MirageWallpaper` 根目录执行，需要 Python 3。

重启独立编辑器并打开指定工程：

```sh
python3 StandaloneEditor/tools/run_standalone.py --project '/完整路径/project.json'
```

停止独立编辑器及其辅助进程：

```sh
python3 StandaloneEditor/tools/run_standalone.py --stop
```

重启或停止会结束当前编辑器会话，请先保存工程。运行日志位于 `runtime/logs/`。

## 内部构建

本节仅适用于已获授权的开发与维护人员。适配层源码和构建工具不以开源许可证提供。

构建依赖：

- Xcode Command Line Tools：Clang、Swift。
- Python 3、Node.js。
- Homebrew LLVM、MinGW-w64、Vulkan Headers。
- 与适配版本匹配的完整 `runtime/` 目录。

运行环境及 `runtime/private/ui-patches.json` 须在有效授权范围内另行准备，不包含在 Git 仓库中。

重建适配层并校验安装结果：

```sh
python3 StandaloneEditor/tools/build_runtime.py
python3 StandaloneEditor/tools/verify_runtime.py
```

构建工具使用 `runtime/originals/` 中的原始输入，生成原生辅助程序、GPU 桥接库和编辑器适配文件。常规双击启动无需安装编译工具。

### Mirage 发行组件

在已有匹配运行环境的维护机上生成运行组件包：

```sh
python3 StandaloneEditor/tools/package_support.py
```

默认使用 `src/EditorAdaptations.json` 中已经验证的转换指令。维护新 UI 版本时，可用 `--original-ui /path/to/Steam/scripts.js --update-adaptations` 迁移锚点；任何锚点不唯一或语法不合法都会拒绝生成。

输出为被 Git 忽略的 `StandaloneEditor/dist/EditorSupport/`。包中仅包含自有适配组件、第三方运行依赖、许可证、校验清单及本地转换指令。原版 WE、包含原版代码的私有补丁清单、已初始化的 Wine prefix、用户配置及工程不会进入组件包。

Mirage 构建步骤通过 `Mirage/scripts/bundle_editor_support.py` 验证、签名并嵌入该包。构建机可设置 `MIRAGE_EDITOR_SUPPORT` 指向已准备的组件目录，或设置 `MIRAGE_REQUIRE_EDITOR_SUPPORT=1` 要求默认目录提供组件。显式配置缺失、校验失败或架构不匹配都会终止构建。未配置组件的干净检出可以构建普通 Mirage；编辑器入口会提示该安装包没有运行组件。

组件目录支持单个架构的 `catalog.json`，或分别放在 `arm64/`、`x86_64/` 子目录的两个组件包。安装器按当前原生进程架构选择组件，不回退到其他架构。

发行包将 Wine 引擎保存在编辑器应用内，保持其运行库和语言数据的相对目录，并对每个应用执行严格签名校验。原生文件选择服务随编辑器进程退出。

安装器按 Steam 清单挂载原版内容，并将原版安装布局 `distribution/` 的内容映射到安装根目录。清单中的 depot/manifest 固定于当次安装计划，下载首先校验编辑器 EXE 与 UI 的 SHA-256，只有精确匹配的版本才应用转换指令。运行依赖随 Mirage 构建发布，WE 本体始终通过用户的 Steam 会话获取。

回归检查：

```sh
dotnet run --project SteamService.Tests
python3 Mirage/Tests/run_editor_installation.py
python3 Mirage/Tests/run_editor_packaging.py
```

完整初始化测试可使用隔离目录中的原版文件：`run_editor_installation.py --assemble-root /path/to/test/Editor --support /path/to/EditorSupport`。原版文件应位于测试目录的 `downloads/` 中；测试会组装安装并验证产物，不使用生产目录。

### Apple Silicon 原生运行组件

ARM64 路线使用原生 Wine + FEX，不使用 Rosetta。macOS 侧的启动器、Wine、辅助服务和 Metal 桥接均为 ARM64；原版 Windows x64 编辑器与 DXVK 的指令由 FEX 执行。当前使用高地址 Wine 变体，最低要求 macOS 26.5，不支持 Windows 32 位程序。

在 Apple Silicon 构建机上运行：

```sh
python3 StandaloneEditor/tools/build_native_arm64.py --install-dependencies
python3 StandaloneEditor/tools/package_support.py --architecture arm64
MIRAGE_EDITOR_SUPPORT="$PWD/StandaloneEditor/dist/EditorSupport/arm64" \
  python3 Mirage/scripts/bundle_editor_support.py /path/to/Mirage.app "$PWD" arm64
python3 StandaloneEditor/tools/verify_native_runtime.py \
  --support /path/to/Mirage.app/Contents/Resources/EditorSupport
```

构建固定 Wine、FEX、补丁集与工具链版本。打包校验每个 Mach-O 均只有 ARM64，重定位第三方动态库，并绑定当前 Wine 音频驱动的源码 ABI 和 UUID。Intel 专用的窗口锁偏移补丁不会用于 ARM64。

`Validate native ARM64 editor runtime` 工作流先验证 x64 指令执行，再对签名后的组件包执行独立设备间的 GPU 共享纹理读回、连续更新、尺寸变化与释放测试。Wine/FEX 编译完成后即缓存构建结果，后续打包或 GPU 验证失败时可复用。全部通过后才上传运行包及相应 Wine/FEX 源码。测试不包含 WE 私有文件或 Steam 会话；完整编辑器工程、CEF 窗口与音频交互仍需在 Apple Silicon 上验收，不能仅凭交叉编译认定可用。

## 许可与商业条款

Copyright © 2026 王孝慈. All rights reserved.

Mirage Editor 的自有代码、适配层和相关文档按专有条款提供，不以 Mirage Wallpaper 主项目的 GPL-3.0 许可证发布。公开可见的自有代码仍保留全部权利；获得源码不构成开源许可或再分发授权。除另有书面授权或适用的第三方许可证明确允许外，不得公开其未发布的私有源码、复制分发、转售或再许可其专有部分。

Wallpaper Engine 的程序、界面代码及资源等专有组件，其权利归各自权利人所有。使用这些组件须遵守适用的 Wallpaper Engine 最终用户许可协议、[Steam 订户协议](https://store.steampowered.com/subscriber_agreement/)及其他商业授权条件；涉及工坊内容时，还须遵守 [Wallpaper Engine 工坊附加条款](https://store.steampowered.com/eula/431960_eula_0)。

商业集成、收费分发、转售或再许可必须取得相关权利人所要求的授权。持有 Wallpaper Engine 的使用许可，不代表获得其专有组件的再分发或商业集成权利；Mirage Editor 的闭源声明或授权也不能替代这些许可。具体范围以相关权利人的有效协议为准。[Steam 订户协议](https://store.steampowered.com/subscriber_agreement/)

Wine、DXVK、MoltenVK 及其他第三方依赖继续适用各自许可证，并须保留相应版权、许可声明及履行适用义务。上述闭源条款不限制第三方许可证已授予的权利。壁纸、模型、字体、音频等工程素材的使用和发布须遵守其各自授权。

Mirage Editor 为非官方适配项目，与 Wallpaper Engine Team、Skutta Software GmbH 或 Valve 无隶属关系，亦未获得其官方背书。
