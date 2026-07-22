---
title: Mirage 是什么
description: 认识 Mirage —— 面向 macOS 的原生动态壁纸管理器与 Wallpaper Engine 兼容运行时。
---

Mirage 是一款面向 macOS 的**原生动态壁纸管理器**，同时也是一个 **Wallpaper Engine 兼容运行时**。它用 SwiftUI 与 AppKit 提供壁纸浏览、管理和系统集成，并通过三个独立的渲染进程分别播放场景、网页和视频壁纸。

你既可以用它读取本地的 Wallpaper Engine 风格壁纸包，也可以直接浏览 Steam 创意工坊、安装 SteamCMD、登录 Steam 并下载壁纸。

![Mirage 实际渲染的场景动态壁纸](/images/product/rendered-scene.webp)

*由 Mirage 的 SceneWallpaper 渲染器实际输出的场景壁纸。*

:::note[项目仍处于早期阶段]
Mirage 正在持续开发，Wallpaper Engine 场景格式的兼容性仍在完善。复杂作品可能在特效、脚本或材质表现上存在差异。遇到问题欢迎[提交 Issue](https://github.com/laobamac/MirageWallpaper/issues/new/choose) 或加入交流群反馈，详见[社区与反馈](/reference/community/)。
:::

## 它能做什么

- **播放三类动态壁纸**：`scene`（场景）、`web`（网页）、`video`（视频）。
- **管理壁纸库**：浏览已安装壁纸，支持搜索、排序，以及按类型、来源、标签、内容分级筛选和收藏。
- **导入与转换**：直接导入包含 `project.json` 的目录，或把 `.mp4`、`.mov`、`.m4v` 视频转换成本地壁纸包。
- **对接 Steam 创意工坊**：浏览趋势、最新、热门、评分和标签分类内容，识别并下载创意工坊预设。
- **托管 SteamCMD**：由 Mirage 管理 SteamCMD 的安装、独立数据目录、Steam 登录和 Steam Guard 验证。
- **系统集成**：多显示器覆盖、菜单栏控制、登录启动、桌面占位图恢复。
- **动态屏保**：把视频、网页、场景壁纸装成独立屏保，保留当前预设与自定义属性。
- **智能省电**：在全屏应用、其他应用播放音频、屏幕休眠或电池供电时，可选择继续、静音、暂停或停止。

## 核心设计：多进程渲染

Mirage 把渲染工作交给三个独立进程完成，主应用只负责界面与调度：

| 组件 | 技术 | 职责 |
| --- | --- | --- |
| Mirage | SwiftUI、AppKit | 界面、壁纸库、创意工坊、设置、进程管理和系统集成 |
| SceneWallpaper | C++20、Vulkan、MoltenVK | `scene.pkg` / `scene.json`、材质、粒子、LUT、文字和用户属性 |
| WebWallpaper | Objective-C++、WKWebView | HTML 壁纸、JavaScript、媒体、鼠标事件和用户属性 |
| VideoWallpaper | Objective-C++、AVFoundation | 视频循环、音量、速度和填充模式 |
| MirageScreenSaver | Swift、WebKit、AVFoundation、Metal | 独立安装的动态屏保宿主 |

渲染器作为独立进程运行。Mirage 通过标准输入发送逐行 JSON 控制消息，因此单个渲染器异常不会直接破坏主应用状态。想深入了解，请阅读[渲染架构](/advanced/architecture/)。

## 接下来

- 先确认你的机器满足[系统与构建要求](/guides/requirements/)。
- 然后按[安装与首次启动](/guides/install/)把 Mirage 跑起来。
- 想快速熟悉界面，看[界面导览](/guides/interface/)。

:::caution[与官方无关]
Mirage 与 Valve、Steam 或 Wallpaper Engine 没有关联，也未获得其官方认可。使用 Steam 创意工坊内容时请遵守相应的许可与条款。
:::
