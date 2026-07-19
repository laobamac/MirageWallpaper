---
title: 隐私与数据
description: Mirage 如何处理你的 Steam 凭据、API Key 与本地数据。
---

Mirage 是一个本地运行的桌面应用。它不运营任何账户系统，也不会把你的使用数据上传到 Mirage 的服务器。本页说明涉及隐私的几个关键点。

## Steam 凭据

- 登录 Steam 时，用户名和密码只交给**本机的 SteamCMD 进程**完成登录，**不会上传到任何第三方服务器**。
- 密码不会被明文长期保存；登录成功后 SteamCMD 在本地隔离目录维护自己的会话。
- Mirage 只持久化你的**用户名**，用于下次复用会话。
- Steam Guard 验证码同样只用于当次登录。

详见 [Steam 登录](/MirageWallpaper/workshop/login/)与 [SteamCMD](/MirageWallpaper/workshop/steamcmd/)。

## Steam Web API Key

- 你填写的 API Key 保存在本地设置中，仅用于**在本机请求创意工坊浏览数据**。
- 请勿公开分享你的 Key。
- App 附带一个所有用户共享的内置 Key，仅作浏览用途。

见 [Steam Web API Key](/MirageWallpaper/workshop/api-key/)。

## 网络请求

Mirage 发起网络请求的场景有限且可预期：

| 用途 | 目标 |
| --- | --- |
| 浏览创意工坊 | Steam Web API（官方或你选择的镜像端点） |
| 下载创意工坊内容 | 通过 SteamCMD 连接 Steam |
| 安装 SteamCMD | Valve 官方 CDN |
| 检查 / 下载更新 | Mirage 的更新源（GitHub Release / appcast） |
| 网页壁纸 | 由壁纸自身决定（见下） |

### 镜像端点

中国大陆用户可选的 SteamCF 镜像**并非 Steam 官方服务**，不保证安全性与可用性，且只代理浏览 API，不加速登录或下载。是否使用由你自行决定。

## 网页壁纸

网页壁纸会执行 JavaScript，可能自行发起网络请求或加载远程资源。Mirage 因此对未信任的网页壁纸设有[安全确认](/MirageWallpaper/settings/web-safety/)。请只信任来源可靠的网页壁纸。

## 诊断日志

- Mirage 的创意工坊支持报告会**自动脱敏**：密码、API Key、令牌等字段会被替换为 `[已隐藏]`。
- 开启「详细日志」会记录更多信息用于调试；导出或分享日志前请自行确认其中不含敏感内容。

## 本地数据

你的壁纸、下载、缓存和设置都保存在本机。相关路径见[数据目录](/MirageWallpaper/advanced/data-directories/)。你可以随时在访达中查看、备份或清理这些目录。

## 关联声明

Mirage 与 Valve、Steam、Wallpaper Engine 没有关联，也未获其官方认可。创意工坊内容归各自作者所有，请遵守相应的许可与使用条款。
