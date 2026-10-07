---
title: 常见问题（FAQ）
description: 关于 MirageLinuxDisplay 的常见问题。
---

## MirageLinuxDisplay 和 MirageWallpaper 是什么关系？

MirageLinuxDisplay 是 MirageWallpaper 生态中的 Linux 桌面显示集成层。它不渲染壁纸，而是定义 `mirage-display-v1` 协议，让 MirageWallpaper 渲染器把 DMA-BUF 帧交给桌面环境（如 KDE Plasma）自有的壁纸表面。

## 为什么不直接创建 X11 桌面窗口？

X11 支持由桌面环境适配器承担。只有 DE 才能可靠维持桌面堆叠、活动、虚拟桌面、壳层输入与重启行为；直接占有根窗口无法与 Plasma/KWin 的壁纸生命周期协作。详见[适配器边界](/adapters/boundary/)。

## 像素数据会经过 broker 拷贝吗？

不会。broker 只转发协议报文与文件描述符，DMA-BUF 像素数据全程留在 GPU 显存。

## 支持哪些桌面环境？

KDE Plasma 6 已实现（X11 与 Wayland 通用）。GNOME Shell 与 layer-shell 通用消费者正在规划中，见[规划中的适配器](/adapters/planned/)。

## 我可以自己写一个消费者吗？

可以。消费库是稳定的 C ABI（`include/mirage_display.h`），兼容 Qt、GObject 与 Rust FFI。参考[消费者库](/dev/consumer/)与[示例](/dev/examples/)。

## 0.1.x 的下游需要重新编译吗？

需要。0.2.0 把跨语言的 FD、超时与状态计数统一为定宽类型，并为公开 DTO 固定了八字节布局，使用 0.1.x 的下游必须重新编译；线上协议保持不变。见[ABI 与版本兼容](/reference/abi/)。
