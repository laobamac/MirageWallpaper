---
title: 故障排查
description: 解决 Mirage 常见问题：壁纸不显示、性能异常、导入失败与更新问题。
---

本页汇总 Mirage 使用中的常见问题。创意工坊相关的登录与下载问题请见[创意工坊故障排查](/MirageWallpaper/workshop/troubleshooting/)。

## 壁纸不显示或黑屏

- 确认壁纸目录根部有 `project.json`，且类型受支持（场景 / 网页 / 视频）。
- 若是刚导入或下载的壁纸，尝试在壁纸库中**刷新**。
- 复杂的 Wallpaper Engine 场景可能存在兼容性差异，换一个作品验证是否为个例。
- 网页壁纸首次应用需要在安全提示中**确认信任**，否则不会显示。见[网页壁纸安全](/MirageWallpaper/settings/web-safety/)。

## 壁纸卡顿或占用高

- 在[性能设置](/MirageWallpaper/settings/performance/)中降低帧率（建议 30）或调低渲染质量。
- 关闭抗锯齿。
- 场景壁纸通常比视频、网页更吃资源。

## 壁纸意外暂停或停止

这通常是**播放规则**在起作用，属于预期的省电行为。检查[性能设置](/MirageWallpaper/settings/performance/)中的规则：

- 其他应用全屏 / 获得焦点 / 播放音频时；
- 显示器睡眠时；
- 笔记本使用电池时。

多个条件同时命中时取最强动作（停止 > 暂停 > 静音 > 保持运行）。

## 多显示器问题

- 每块屏幕可独立设置壁纸，确认你操作的是目标屏幕。
- 插拔显示器或改变排列后，可刷新壁纸库或重新应用。详见[多显示器](/MirageWallpaper/wallpapers/displays/)。

## 导入失败

- 确认视频为常见格式（`.mp4` / `.mov` / `.m4v`）。
- 确认导入目录可写；可在[通用设置](/MirageWallpaper/settings/general/)中查看或更改导入目录。
- 详见[导入本地壁纸](/MirageWallpaper/formats/import/)。

## 声音问题

- 检查[通用设置](/MirageWallpaper/settings/general/)中的全局音量与全局静音。
- 检查该壁纸自身的[音量设置](/MirageWallpaper/wallpapers/playback/)。
- 屏保始终静音，这是设计如此。

## 更新问题

- 若自动更新关闭，用菜单栏「检查更新…」手动检查。
- 测试版需在[软件更新设置](/MirageWallpaper/settings/updates/)中开启。
- 首次安装可能需要在 macOS Gatekeeper 中手动允许 Mirage 运行。

## 屏保不生效

- 在[屏保设置](/MirageWallpaper/settings/screensaver/)确认已**安装**组件。
- 安装后仍需在**系统设置**的屏保面板中选中 Mirage。
- 更新 Mirage 后若屏保行为异常，点「重新安装」同步到最新版本。

## 收集诊断信息

- 在[通用设置](/MirageWallpaper/settings/general/)开启「详细日志」以获得更多信息。
- 创意工坊问题可导出脱敏的支持报告。
- 反馈问题时附上系统版本、Mirage 版本和复现步骤，见[社区与反馈](/MirageWallpaper/reference/community/)。
