---
title: 数据目录
description: Mirage 的壁纸来源目录、SteamCMD 目录、缓存和设置的存放位置。
---

Mirage 会从多个来源扫描壁纸，并把下载、缓存和设置分别放在不同位置。了解这些路径有助于备份、清理和排查问题。

## 壁纸来源目录

Mirage 的壁纸库同时扫描以下来源（在[通用设置](/settings/general/)的「壁纸库」区块可查看真实路径并「在访达中显示」）：

| 来源 | 说明 |
| --- | --- |
| Steam 创意工坊目录 | 系统 Steam 的默认创意工坊内容目录（App ID `431960`） |
| 自定义创意工坊目录 | 你手动指定的 Wallpaper Engine 内容目录（设置后，系统默认目录降为兼容项） |
| Mirage 下载目录 | SteamCMD 当前下载和更新壁纸的位置（标注「当前下载位置」） |
| Mirage 旧版下载目录 | 仅用于兼容旧版本已下载的壁纸 |
| 导入壁纸目录 | 手动导入或由视频创建的本地壁纸 |

只有**根目录包含 `project.json`** 的子目录才会被识别为壁纸。

## SteamCMD 目录

Mirage 托管的 SteamCMD 使用独立目录，位于应用支持目录下：

```text
~/Library/Application Support/Mirage/steamcmd/
├── .mirage-ready                       # 安装完成标记
├── home/                               # 隔离的 HOME
│   └── Library/Application Support/Steam/
│       ├── config/config.vdf           # SteamCMD 会话
│       └── steamapps/workshop/content/431960/   # 当前下载目录
└── steamapps/workshop/content/431960/  # 旧版下载目录（兼容）
```

该目录权限为仅当前用户可访问（`0700`）。详见 [SteamCMD 与 Steam 登录](/workshop/steamcmd/)。

## 缓存

创意工坊浏览的缩略图与数据缓存在缓存目录：

```text
~/Library/Caches/Mirage/WorkshopCache/
```

清理缓存不会影响已下载的壁纸，只会让下次浏览重新拉取。

## 屏保组件

动态屏保安装到当前用户的屏保目录：

```text
~/Library/Screen Savers/MirageScreenSaver.saver
~/Library/Application Support/Mirage/screensaver.json   # 屏保配置
```

见[动态屏保](/screensaver/overview/)。

## 设置与运行时状态

- **全局设置**、信任的网页壁纸、SteamCMD 用户名等保存在应用的 `UserDefaults`（偏好设置）。
- **每个壁纸的运行时状态**（音量、速度、填充、自定义属性等）以 `Runtime_<id>` 键保存在 `UserDefaults`。

## 自定义目录

你可以在[通用设置](/settings/general/)中为「Steam 创意工坊目录」和「导入壁纸目录」指定自定义位置，也可随时「恢复默认」。

:::caution[清理需谨慎]
删除下载目录或导入目录会移除对应的壁纸；删除 SteamCMD 目录会导致需要重新安装并重新登录。备份前请确认目录内容。
:::
