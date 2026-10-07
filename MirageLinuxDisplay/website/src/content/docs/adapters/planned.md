---
title: 规划中的适配器
description: GNOME Shell、layer-shell 通用消费者与 MirageWallpaper X11 所有权的移除计划。
---

以下适配器在仓库中**尚无实现**，均处于规划阶段：

## GNOME Shell

规划为一个 Shell 扩展加原生纹理导入助手：把纹理插入 Shell 自有的背景 actor，在观察舞台事件时返回 `Clutter.EVENT_PROPAGATE`，并从 Shell/Mutter 接口读取窗口与工作区状态。如果某个 GNOME X11 版本暴露了所需 Shell API，就沿用同一扩展路径，不引入根窗口渲染器。

## 通用合成器（layer-shell）

只有合成器没有桌面壳层壁纸 API 时，才允许使用通用适配器。Wayland layer-shell 是首选回退；如果合成器自有 API 能提供更好的输出标识、输入观察或壁纸生命周期语义，就优先于 layer-shell。

刻意不提供通用 X11 回退：X11 支持由桌面环境适配器承担，因为只有 DE 才能可靠维持桌面堆叠、活动、虚拟桌面、壳层输入与重启行为。

## 移除 MirageWallpaper 对直接 X11 的所有权

等上游渲染器接入 `mirage-display-v1` 生产者路径后，移除 MirageWallpaper 对裸 X11 桌面的直接所有权。

## 相关

- [适配器边界](/adapters/boundary/)
- [架构总览](/guides/architecture/)
