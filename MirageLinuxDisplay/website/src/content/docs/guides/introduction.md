---
title: MirageLinuxDisplay 是什么
description: 认识 MirageLinuxDisplay —— 为 MirageWallpaper 打造的 Linux 桌面环境显示集成层。
---

MirageLinuxDisplay 是 [MirageWallpaper](https://github.com/laobamac/MirageWallpaper) 在 Linux 桌面上的**显示集成层**。它定义了 `mirage-display-v1` Unix 域套接字协议，并提供稳定的 C ABI 消费库、生产库与路由核心；KDE Plasma 等桌面环境通过这些库接收 MirageWallpaper 渲染出的 DMA-BUF 帧，再把桌面输入回传给渲染器。

MirageWallpaper 的 Linux 版本不再"直接占有 X11 桌面"，而是改用**协议驱动的离屏渲染**：渲染器导出 DMA-BUF 帧，桌面环境集成负责在 DE 自有的壁纸表面上显示，并把指针输入回传给渲染器。

![MirageLinuxDisplay 架构数据流](/MirageLinuxDisplay/images/architecture.svg)

*渲染器 → broker → libmirage-display → 桌面环境壁纸表面。像素数据全程留在 GPU 显存。*

## 能力边界

X11 与 Wayland 会话都受支持，但两种会话都通过桌面环境自有的集成点呈现。本项目**不创建、也不管理**裸的 X11 桌面或根窗口。例如 KDE 适配器是一个 Plasma 壁纸插件，在 Plasma X11 与 Plasma Wayland 上以相同方式运行，工作区信息取自 Plasma/KWin 接口。

这条边界是强制的：消费者不得用 Xlib/XCB 桌面窗口或私有 Wayland toplevel 来替代 DE 的壁纸表面。详见[适配器边界](/adapters/boundary/)。

## 已实现能力

- **协议**：`mirage-display-v1` 已经冻结，以 `protocol/mirage_display_v1.xml` 为权威定义，覆盖握手、输出注册、缓冲池绑定、帧与同步、配置、指针输入与错误处理。
- **路由核心**（`mirage_display_broker.h`）：以权限 `0600` 的 Unix 域套接字提供服务，通过 `SO_PEERCRED` 校验对端是否同 UID，按稳定输出标识把一个生产者与多个 DE 消费者匹配起来，协商格式/修饰符组合，并转发 DMA-BUF 与同步描述符。
- **消费库**（`mirage_display.h`）：提供非阻塞握手、缓冲池生命周期、帧接收、显式同步、延迟解绑与指针/窗口状态上报，兼容 Qt、GObject 与 Rust FFI。
- **生产库**（`mirage_display_producer.h`）：负责渲染端会话、缓冲出借、帧提交与同步对象管理。
- **GPU 助手**：EGL（`EGL_EXT_image_dma_buf_import`）与 Vulkan（external memory FD / DRM 修饰符）导入、同设备 relay/blit 回退，以及 DRM syncobj 扇出与释放。
- **KDE Plasma 适配器**：Qt Quick 显示项 + Plasma 壁纸包，提供 OpenGL/EGL 与 Vulkan 双后端，支持多显示器、旋转、缩放、指针转发与窗口状态。

尚未实现的部分（GNOME Shell 适配器、layer-shell 通用消费者）见[规划中的适配器](/adapters/planned/)。

:::note[项目仍处于早期阶段]
MirageLinuxDisplay 正在持续开发。遇到问题欢迎提交 [GitHub Issue](https://github.com/elysia-best/MirageLinuxDisplay/issues/new/choose)，并说明系统版本、复现步骤、预期结果与实际现象。
:::

## 接下来

- 先了解[整体架构](/guides/architecture/)与数据流。
- 再按[构建与测试](/guides/build/)把核心库跑起来。
- 想接入桌面环境，可以看[KDE Plasma 适配器](/adapters/kde/)。
- 想基于 C ABI 开发，请阅读[消费者库](/dev/consumer/)与[生产者库](/dev/producer/)。
