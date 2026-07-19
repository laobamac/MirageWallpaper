---
title: 壁纸类型
description: Mirage 支持的场景、网页、视频三类动态壁纸，以及各自的渲染方式。
---

Mirage 通过 `project.json` 中的 `type` 字段识别壁纸类型，并交给对应的渲染进程播放。三种主要类型如下。

## 场景（scene）

场景壁纸是 Wallpaper Engine 的原生格式，通常包含 `scene.json` 或打包后的 `scene.pkg`，以及材质、模型、粒子、着色器等资源。

- **渲染器**：SceneWallpaper
- **技术栈**：C++20、Vulkan（通过 MoltenVK 转译为 Metal）
- **能力**：材质、粒子、LUT 调色、文字渲染、用户属性

:::note[兼容性仍在完善]
场景格式最为复杂，Mirage 对 Wallpaper Engine 场景的兼容性仍在持续完善。含有复杂脚本、特殊着色器或高级特效的作品可能表现存在差异。
:::

## 网页（web）

网页壁纸本质上是一个 HTML 页面，可以运行 JavaScript、播放媒体、响应鼠标事件。

- **渲染器**：WebWallpaper
- **技术栈**：Objective-C++、WKWebView
- **能力**：HTML / CSS / JS、媒体播放、鼠标交互、用户属性

由于网页壁纸会执行脚本，应用来源不明的网页壁纸时 Mirage 会弹出安全确认。详见[网页壁纸安全](/settings/web-safety/)。

## 视频（video）

视频壁纸循环播放一段视频，是最轻量、兼容性最好的类型。你可以[直接把视频转换成视频壁纸](/formats/video-convert/)。

- **渲染器**：VideoWallpaper
- **技术栈**：Objective-C++、AVFoundation
- **能力**：循环播放、音量、速度、填充模式

## 其它类型

`project.json` 中还可能出现 `application`（应用程序）等类型。Mirage 在筛选中提供「应用程序」和「预设」分类，但对无法识别或不受支持的类型不会启动渲染进程。关于创意工坊预设，见[预设](/workshop/presets/)。

## 类型与渲染架构

每种类型对应一个独立的渲染进程，彼此隔离运行。想了解进程如何通信、崩溃如何隔离，请阅读[渲染架构](/advanced/architecture/)。
