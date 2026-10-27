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

## 预览

预览适配层复用 GPU 程序和共享纹理，避免重复尺寸通知；画布不可见或销毁后停止重绘，并支持图形上下文恢复。显示画布不再额外申请深度、模板和多重采样缓冲，场景自身的分辨率、抗锯齿和效果设置不变。CEF 的 Metal 图层使用异步呈现，保留场景的正常提交与帧率控制。

Metal 编译适配会识别可证明只读的双声道 64 项频谱数组，直接读取对应的常量缓冲，保留动态索引与边界检查。不匹配的 shader 使用原始路径；优化后的编译失败时也会回退。优化版本写入 Metal 源码以区分编译缓存，无需清理工程或 WE assets 中的 shader 缓存。

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

## 许可与商业条款

Copyright © 2026 王孝慈. All rights reserved.

Mirage Editor 的自有代码、适配层和相关文档按专有条款提供，不以 Mirage Wallpaper 主项目的 GPL-3.0 许可证发布。公开可见的自有代码仍保留全部权利；获得源码不构成开源许可或再分发授权。除另有书面授权或适用的第三方许可证明确允许外，不得公开其未发布的私有源码、复制分发、转售或再许可其专有部分。

Wallpaper Engine 的程序、界面代码及资源等专有组件，其权利归各自权利人所有。使用这些组件须遵守适用的 Wallpaper Engine 最终用户许可协议、[Steam 订户协议](https://store.steampowered.com/subscriber_agreement/)及其他商业授权条件；涉及工坊内容时，还须遵守 [Wallpaper Engine 工坊附加条款](https://store.steampowered.com/eula/431960_eula_0)。

商业集成、收费分发、转售或再许可必须取得相关权利人所要求的授权。持有 Wallpaper Engine 的使用许可，不代表获得其专有组件的再分发或商业集成权利；Mirage Editor 的闭源声明或授权也不能替代这些许可。具体范围以相关权利人的有效协议为准。[Steam 订户协议](https://store.steampowered.com/subscriber_agreement/)

Wine、DXVK、MoltenVK 及其他第三方依赖继续适用各自许可证，并须保留相应版权、许可声明及履行适用义务。上述闭源条款不限制第三方许可证已授予的权利。壁纸、模型、字体、音频等工程素材的使用和发布须遵守其各自授权。

Mirage Editor 为非官方适配项目，与 Wallpaper Engine Team、Skutta Software GmbH 或 Valve 无隶属关系，亦未获得其官方背书。
