---
title: 常见问题
description: 关于 Mirage 安装、壁纸、创意工坊、性能和更新的常见疑问。
---

## 通用

### Mirage 是免费的吗？

是。Mirage 会继续免费开放开发，遵循 GPL-3.0 许可证。如果它为你的桌面带来了价值，可以按自己的意愿在[社区页面](/reference/community/)列出的渠道赞助；赞助完全自愿，不影响任何功能使用。

### Mirage 支持哪些系统？

Intel（`x86_64`）或 Apple Silicon（`arm64`）Mac，macOS 14.2 或更高版本。详见[系统要求](/guides/requirements/)。

### Mirage 是 Steam 官方客户端吗？

不是。Mirage 并非 Steam 官方客户端，也不隶属于 Valve 或 Wallpaper Engine。它通过 Steam 官方 Web API 浏览创意工坊，通过 Valve 官方命令行工具 SteamCMD 下载内容。

## 壁纸

### Mirage 支持哪些壁纸类型？

三类：场景（`scene`）、网页（`web`）、视频（`video`）。每个壁纸目录以 `project.json` 为入口。详见[壁纸类型](/formats/wallpaper-types/)。

### 为什么有些 Wallpaper Engine 场景表现不正常？

Mirage 对 Wallpaper Engine 场景格式的兼容性仍在完善。复杂作品可能存在特效、脚本或材质表现差异。遇到问题时欢迎提交 Issue 并附上作品信息。

### 我能把自己的视频做成壁纸吗？

可以。把 `.mp4`、`.mov` 或 `.m4v` 文件导入 Mirage，它会自动生成 `project.json` 和预览图。详见[视频转壁纸](/formats/video-convert/)。

### 壁纸的音量和播放速度会记住吗？

会。每个壁纸的音量、速度、静音和填充方式都会单独保存，下次应用时自动恢复。详见[播放控制](/wallpapers/playback/)。

## 创意工坊

### 浏览创意工坊需要什么？

需要一个 Steam Web API Key。App 内置一个共享 Key 供首次浏览使用，但建议[设置自己的 Key](/workshop/api-key/) 以避免共享额度繁忙。

### 下载创意工坊内容需要付费吗？

需要一个拥有 Wallpaper Engine 的 Steam 账户。所有权与项目访问权限会在首次下载时由 Steam 验证。Mirage 通过 SteamCMD 下载，不需要 API Key。

### 为什么下载是一个一个进行的？

同一个交互式 SteamCMD 会话一次只能可靠处理一个创意工坊命令，因此下载队列当前串行执行。详见[下载壁纸](/workshop/download/)。

### 什么是「预设」？

预设只保存属性与附带素材，并依赖一个基础壁纸。基础壁纸已安装时，点击预设会直接应用；尚未安装时，Mirage 会询问是否一起下载。详见[预设与依赖](/workshop/presets/)。

## 性能

### 动态壁纸很耗电吗？

取决于壁纸类型、帧率和质量设置。你可以在[性能设置](/settings/performance/)中设置在电池供电、其他应用全屏、显示器休眠等情况下自动暂停或停止壁纸，以节省资源。

### 帧率应该设多少？

30 FPS 是均衡的默认值。超过 30 会增加耗电，超过 60 会显著增加耗电与占用。

## 更新与屏保

### Mirage 如何更新？

通过 Sparkle 自动检查并下载更新。可以在[软件更新设置](/settings/updates/)中关闭自动更新或开启测试版通道。

### 屏保需要 Mirage 一直运行吗？

不需要。屏保组件被复制到 `~/Library/Screen Savers`，独立运行。详见[屏保](/screensaver/overview/)。
