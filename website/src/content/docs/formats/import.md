---
title: 导入本地壁纸
description: 从文件夹或视频文件导入壁纸到 Mirage 的本地壁纸库。
---

除了从 Steam 创意工坊下载，你也可以直接把本地内容导入 Mirage。导入支持两种输入：**含 `project.json` 的壁纸文件夹**，以及**视频文件**。

## 打开导入面板

有几种方式可以开始导入：

- 主窗口壁纸库底部的导入入口。
- [菜单栏](/MirageWallpaper/wallpapers/menubar/)中的「导入壁纸…」（`I`）。

导入面板允许同时选择多个文件夹或视频文件，支持的类型包括文件夹、以及 `.mp4`、`.mov`、`.m4v` 视频。

## 导入壁纸文件夹

选择文件夹时，Mirage 要求该文件夹**根目录包含 `project.json`**。这正是 Wallpaper Engine 作品的标准结构。满足条件后，整个文件夹会被复制到导入目录。

如果所选文件夹不含 `project.json`，导入会失败并提示「所选文件夹需包含 project.json，请确认后重试。」关于该文件的字段，见 [project.json 结构](/MirageWallpaper/formats/project-json/)。

## 导入视频文件

选择 `.mp4`、`.mov` 或 `.m4v` 视频时，Mirage 会自动把它包装成一个视频壁纸包。这一过程不会重新编码视频，具体行为见[视频转换为壁纸](/MirageWallpaper/formats/video-convert/)。

## 命名冲突

如果导入目标名称已存在，Mirage 会自动追加序号（如 `名称_1`、`名称_2`）避免覆盖已有壁纸。

## 导入后

导入完成后，Mirage 会刷新壁纸库。导入的内容会归入「导入壁纸目录」这一来源，可在[筛选面板](/MirageWallpaper/wallpapers/library/)中按来源定位，实际路径见[数据目录](/MirageWallpaper/advanced/data-directories/)。

:::note
导入是**复制**而非移动或引用，原始文件保持不变。因此导入较大的作品会占用额外磁盘空间。
:::
