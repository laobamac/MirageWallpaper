---
title: SteamCMD 与 Steam 登录
description: Mirage 如何托管 SteamCMD、隔离数据目录，以及处理 Steam 登录与凭据。
---

Mirage 使用 Valve 官方的命令行工具 **SteamCMD** 来下载创意工坊内容。整个过程由 Mirage 托管，你通常不需要手动操作命令行。

## 安装与检测

在[设置向导](/MirageWallpaper/workshop/setup-wizard/)中，Mirage 会：

1. 先**检测**是否已有可用的 SteamCMD。
2. 若没有，从 `steamcdn-a.akamaihd.net` 下载官方 `steamcmd_osx.tar.gz` 并安装到独立目录。

SteamCMD 被安装在应用支持目录下的 `Mirage/steamcmd`，并设置为仅当前用户可访问（权限 `0700`）。安装完成后会写入 `.mirage-ready` 标记，供后续启动快速识别。

## 隔离的数据目录

为了不污染系统里可能已有的 Steam，Mirage 让 SteamCMD 使用**独立的 HOME 和数据目录**：

- SteamCMD 主目录：`Mirage/steamcmd`
- 隔离的 Steam 数据：`Mirage/steamcmd/home/Library/Application Support/Steam`

创意工坊内容（App ID `431960`）会下载到该独立目录下的 `steamapps/workshop/content/431960`。Mirage 的壁纸库会同时扫描当前下载目录、旧版下载目录，以及系统 Steam 的默认目录，完整清单见[数据目录](/MirageWallpaper/advanced/data-directories/)。

## Steam 登录

下载创意工坊内容需要一个已登录的 Steam 账号。

- 你在向导中输入用户名和密码，Mirage 把它们交给 SteamCMD 完成登录；密码不会以明文形式长期保存。
- 登录成功后，SteamCMD 会在隔离目录写入自己的会话配置（`config/config.vdf`）。Mirage 据此判断是否已登录，并在下载时自动复用会话。
- Mirage 只持久化你的**用户名**，用于下次复用会话；会话本身由 SteamCMD 维护。

## Steam Guard

如果账号开启了 Steam Guard：

- **验证码方式**：向导会提示输入邮箱 / 令牌验证码。
- **手机确认方式**：Mirage 进入等待状态，并在日志中每 5 秒提示仍在等待，你在 Steam 手机 App 上确认即可。

## 会话复用与失效

本机已有有效会话时，向导会提供「使用已保存会话」，无需再次输入密码。若会话过期或失效，Mirage 会提示你重新登录。

## 隐私与脱敏

Mirage 会记录一份**已脱敏**的支持诊断日志，用于排查创意工坊相关问题。日志会自动隐藏密码、API Key、令牌等敏感字段（替换为 `[已隐藏]`），并保留最近的若干条事件。你可以导出这份报告用于反馈，详见[故障排查](/MirageWallpaper/workshop/troubleshooting/)。

:::caution
请仅在自己的设备上登录自己的 Steam 账号，并妥善保管凭据。Mirage 不会把你的密码上传到任何第三方服务器；登录只发生在本机的 SteamCMD 进程中。
:::
