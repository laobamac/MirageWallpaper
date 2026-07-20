---
title: 创意工坊概览
description: 了解 Mirage 如何对接 Steam 创意工坊，以及使用前需要准备什么。
---

Mirage 内置了 Steam 创意工坊的浏览与下载能力，直接对应 Wallpaper Engine（App ID `431960`）的创意工坊内容。你可以在应用里浏览海量壁纸，并由 Mirage 托管的 SteamCMD 完成下载。

![Mirage 中按自然标签浏览的 Steam 创意工坊](/images/docs/workshop-nature.webp)

*实际登录后的创意工坊界面；图中使用「自然」标签筛选。*

## 工作方式

创意工坊功能由两部分协作：

- **Steam Web API**：用于浏览、搜索和获取物品详情。Mirage 通过它拉取趋势、最新、热门、评分和标签分类的内容。参见[Steam Web API Key](/workshop/api-key/)。
- **SteamCMD**：Valve 官方的命令行工具，用于实际下载创意工坊物品。Mirage 会管理它的安装、独立数据目录和 Steam 登录。参见[SteamCMD 与 Steam 登录](/workshop/steamcmd/)。

## 使用前的准备

首次切换到「创意工坊」标签时，Mirage 会引导你完成一个四步设置向导：

1. **欢迎与说明**
2. **安装 / 检测 SteamCMD**
3. **登录 Steam**（支持 Steam Guard，可复用已保存会话）
4. **完成**

向导细节见[创意工坊设置向导](/workshop/setup-wizard/)。

## 浏览与下载

设置完成后，你可以：

- 按**热门趋势 / 最新发布 / 订阅最多 / 评分最高**排序浏览。
- 按标签分类（动漫、自然、赛博朋克、游戏等）筛选。
- 搜索关键字。
- 查看物品详情，然后下载到本地壁纸库。

详见[浏览创意工坊](/workshop/browse/)和[下载与管理](/workshop/download/)。

## API 端点

Mirage 提供两个 Steam Web API 端点：**官方**（`api.steampowered.com`）和**镜像**。在中国大陆网络环境下访问官方端点可能不稳定，可在[设置](/settings/general/)中切换到镜像端点。

:::caution[遵守条款]
创意工坊内容归各自作者所有。请遵守 Steam 与作者的许可与使用条款。Mirage 与 Valve、Steam 或 Wallpaper Engine 没有关联，也未获其官方认可。
:::
