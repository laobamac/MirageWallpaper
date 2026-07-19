---
title: 独立调试渲染器
description: 从命令行单独运行 SceneWallpaper、WebWallpaper、VideoWallpaper，并通过标准输入发送 JSON 控制消息进行调试。
---

Mirage 的三个渲染器都是独立的可执行文件，主程序通过 `fork` 子进程 + 标准输入逐行 JSON 的方式驱动它们。这个设计让渲染器可以脱离主程序单独运行，方便定位「究竟是渲染器本身的问题，还是主程序的调度问题」。

:::note[这一页面向开发者]
如果你只是使用 Mirage，不需要读这一页。它适合想要排查渲染异常、或基于源码做二次开发的人。
:::

## 渲染器二进制在哪里

主程序按「优先用 App 内置，找不到再回退到源码构建目录」的顺序查找渲染器：

| 类型 | 源码构建产物路径（开发回退） |
| --- | --- |
| 场景 | `SceneRenderer/build/<preset>/Tools/SceneWallpaper/SceneWallpaper` |
| 网页 | `WebRenderer/build/release/Tools/WebWallpaper/WebWallpaper` |
| 视频 | `VideoRenderer/build/release/Tools/VideoWallpaper/VideoWallpaper` |

正式打包的 App 会把它们复制进 `Mirage.app` 内部；开发时如果没有内置副本，主程序会根据源文件的编译期路径推算仓库根目录，回退到上面的构建目录。这意味着**只要你在仓库里构建过渲染器，从 Xcode 直接运行主程序就能加载到最新的渲染器**，不必每次重新打包。

## 控制协议

渲染器接收的是**逐行 JSON**：每一行是一个完整的 JSON 对象，以换行符 `\n` 结尾。主程序通过子进程的标准输入把这些命令写进去；渲染器也会通过标准输出/标准错误回传日志与状态行。

因为通道是标准输入，你完全可以在终端里手动驱动一个渲染器——把 JSON 命令一行行喂给它即可。

:::caution[协议字段以源码为准]
控制消息的具体字段（键名、取值范围）会随版本演进。这一页只说明「怎么连上渲染器」这件不变的事；**每个命令的准确字段请以对应渲染器源码里的解析逻辑为准**，不要照抄任何固定示例，以免与当前版本不一致。
:::

## 手动运行一个渲染器

以视频渲染器为例，先构建，再直接运行：

```bash
# 构建（release）
./VideoRenderer/scripts/build.sh release

# 直接运行，并从终端逐行发送 JSON 控制消息
./VideoRenderer/build/release/Tools/VideoWallpaper/VideoWallpaper
```

进程启动后，它会在标准输入上等待逐行 JSON。你可以：

- 手动逐行输入 JSON 命令，观察渲染器的输出与窗口行为；
- 或者用脚本把一串命令按行喂进去：

```bash
printf '%s\n' '<第一条 JSON 命令>' '<第二条 JSON 命令>' \
  | ./VideoRenderer/build/release/Tools/VideoWallpaper/VideoWallpaper
```

场景渲染器（`SceneWallpaper`）还依赖运行时资源目录 `assets/`（着色器、材质、字体等）。主程序会优先使用 App 内置的 `assets`，回退到仓库根目录的 `assets`。手动运行场景渲染器时，请确保它能找到这份资源目录。

:::danger[不要改动 `assets/` 下的内容]
`assets/` 目录里包含场景运行库所需的资源与测试壁纸。调试时只读取、不要修改其中的文件，以免破坏渲染器的运行环境。
:::

## 观察主程序如何驱动渲染器

如果你想看主程序实际下发了哪些命令，而不是自己手动构造，可以：

1. 在「设置 → 关于 / 开发者相关」中把日志级别调高（详见 [设置总览](/settings/overview/)）。
2. 从 Xcode 运行主程序，在控制台观察 `[Mirage] 启动渲染器: …` 一类的日志，确认加载的是哪一个二进制、作用在哪块屏幕。
3. 渲染器自身的标准输出/标准错误会被主程序转发，出现在同一个日志流里。

## 为什么要拆成独立进程

- **隔离崩溃**：单个渲染器异常退出，不会直接拖垮主程序的状态；主程序可按策略重启或回退到占位图。
- **按屏幕独立**：多显示器时，每块屏幕可以运行各自的渲染器实例，互不干扰。
- **便于调试**：可以脱离主程序单独复现问题，把「渲染」与「调度」两层解耦。

关于整体进程模型，见 [渲染架构](/advanced/architecture/)。
