---
title: 创意工坊设置向导
description: 首次使用创意工坊时的四步设置流程：SteamCMD、Steam 登录到完成。
---

首次切换到「创意工坊」标签时，Mirage 会打开一个四步向导，帮你把创意工坊功能准备就绪。你可以随时返回上一步，未完成的操作（安装、登录）会被安全取消。

## 第 1 步：欢迎

介绍创意工坊功能会用到 SteamCMD 和 Steam 登录，说明用途。此步骤可直接继续。

## 第 2 步：SteamCMD

Mirage 会先**检测**系统中是否已有可用的 SteamCMD：

- **已找到**：直接使用现有的 SteamCMD。
- **未找到**：由 Mirage 自动**安装**。它会从 Valve 官方地址下载 `steamcmd_osx.tar.gz` 并部署到独立目录。

安装过程中可以取消。只有检测到或安装完成后才能进入下一步。SteamCMD 的存放位置和隔离机制见 [SteamCMD 与 Steam 登录](/workshop/steamcmd/)。

## 第 3 步：Steam 登录

下载创意工坊内容需要登录 Steam 账号。

- 输入 **Steam 用户名和密码**后登录。密码仅用于向 SteamCMD 完成登录，不会明文存储。
- 若账号开启了 **Steam Guard**，会提示输入验证码；使用**手机确认**时，Mirage 会在日志中每 5 秒提示仍在等待，你在手机上确认即可。
- 如果本机已有**验证有效的 SteamCMD 会话**，向导会提供「使用已保存会话」，无需重新输入密码。

登录日志会实时显示，遇到问题可据此排查。登录成功后才能进入最后一步。关于隐私与凭据处理，见[SteamCMD 与 Steam 登录](/workshop/steamcmd/)。

## 第 4 步：完成

确认设置完成，Mirage 会记住你的用户名以便下次复用会话。之后即可正常[浏览](/workshop/browse/)和[下载](/workshop/download/)创意工坊内容。

## 重新运行向导

如果之后需要重新登录或更换账号，可以再次进入创意工坊设置。保存的会话失效时，Mirage 会提示你用密码重新登录。

:::tip[建议先配置 API Key]
向导主要处理 SteamCMD 与登录。浏览用的 Steam Web API 有一个所有用户共享的内置 Key，容易繁忙。建议额外填写你自己的 [Steam Web API Key](/workshop/api-key/)，浏览体验会更稳定。
:::
