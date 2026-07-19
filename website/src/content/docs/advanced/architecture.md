---
title: 渲染架构
description: Mirage 的多进程渲染模型：主程序、三个独立渲染器与它们之间的通信。
---

Mirage 采用**多进程架构**：SwiftUI 主程序负责界面与调度，实际的壁纸渲染交给三个独立的渲染器进程。这样单个壁纸崩溃不会拖垮整个应用，也让不同技术栈各司其职。

## 组件划分

| 组件 | 语言 / 框架 | 职责 |
| --- | --- | --- |
| Mirage.app | Swift / SwiftUI | 主界面、壁纸库、创意工坊、设置、进程调度 |
| SceneWallpaper | C++20 / Vulkan（经 MoltenVK → Metal） | 渲染场景壁纸 |
| WebWallpaper | Objective-C++ / WKWebView | 渲染网页壁纸 |
| VideoWallpaper | Objective-C++ / AVFoundation | 渲染视频壁纸 |

三个渲染器分别来自仓库中的 `SceneRenderer`、`WebRenderer`、`VideoRenderer` 子项目，构建后作为可执行文件被主程序调用。

## 进程调度

主程序中的渲染控制器（RendererController）负责启动、切换和停止渲染器进程：

- 按壁纸类型选择对应的渲染器二进制。
- **每个屏幕**可运行独立的渲染进程，实现不同显示器显示不同壁纸。
- 切换壁纸时采用「候选 → 就绪 → 替换」的流程：新渲染器先在后台准备好第一帧，再替换正在显示的旧进程，避免切换时露出空白桌面。

## 进程间通信

主程序通过**标准输入的 JSON 行协议**向渲染器下发命令：每行一条 JSON 指令，用于设置壁纸、调整属性、音量、速度、填充方式、帧率等。渲染器通过标准输出/标准错误回传状态与日志，主程序逐行解析。

## 崩溃隔离与恢复

- 渲染器是独立进程，崩溃只影响单个壁纸，主程序和其它屏幕不受影响。
- 主程序监听渲染器退出事件，可据此做清理或按策略重启。
- [性能设置](/settings/performance/)中的「停止（释放内存）」等策略，实际就是终止对应的渲染进程以释放资源。

## 二进制查找

渲染控制器优先使用**打包进 App**的渲染器二进制；在开发环境下，若 App 内没有，则回退到各子项目的 `build/` 产物（dev fallback），方便边改边测。

- 场景渲染器还依赖 **MoltenVK**（Vulkan → Metal 转译层）及其 ICD 配置，优先用打包内的，其次回退到 Homebrew 安装位置。

## 相关

- 构建这些组件见[从源码构建](/advanced/build/)。
- 单独调试某个渲染器见[调试渲染器](/advanced/debug-renderers/)。
- 各类型壁纸的能力对比见[壁纸类型](/formats/wallpaper-types/)。
