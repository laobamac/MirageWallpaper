---
title: 创意工坊故障排查
description: 解决创意工坊浏览、登录和下载中常见的问题。
---

创意工坊功能涉及网络、Steam Web API 和 SteamCMD 多个环节。下面按现象整理了常见问题与处理办法。

## 浏览相关

**一直提示繁忙、超时或加载不出内容**

- 多半是共享的内置 Steam Web API Key 被限流。填写你自己的 [Steam Web API Key](/MirageWallpaper/workshop/api-key/) 通常能解决。
- 国内网络访问官方端点不稳定时，可在[设置](/MirageWallpaper/settings/general/)中切换到**镜像端点**。

**提示 API Key 无效**

- 确认 Key 是 32 位十六进制字符串，且没有多余空格。格式无效时 Mirage 会回退到内置 Key。

## 登录相关

**登录失败**

- 核对用户名和密码。
- 若开启了 Steam Guard，注意区分验证码方式和手机确认方式：手机确认需要在 Steam 手机 App 上点确认。
- 查看向导中的登录日志，里面通常有具体原因。

**手机确认一直等待**

- Mirage 会每 5 秒提示仍在等待。请在 Steam 手机 App 中完成确认；长时间无响应可取消后重试。

**提示会话已失效**

- 保存的会话过期后需要用密码重新登录。重新运行[设置向导](/MirageWallpaper/workshop/setup-wizard/)即可。

## SteamCMD 相关

**检测不到 SteamCMD**

- 让 Mirage 自动安装即可。安装会从 Valve 官方地址下载并部署到独立目录。

**安装失败**

- 检查网络是否能访问 `steamcdn-a.akamaihd.net`。
- 可取消后重试安装。

## 下载相关

**下载卡住或失败**

- 确认 Steam 登录仍然有效。
- 检查网络连接。
- 取消该任务后重新下载；重新下载会拉取最新版本。

**预设无法显示**

- 预设需要底层依赖作品。参见[预设](/MirageWallpaper/workshop/presets/)，让 Mirage 自动下载依赖。

## 导出诊断报告

Mirage 会记录一份**已脱敏**的创意工坊支持报告，自动隐藏密码、API Key、令牌等敏感字段。反馈问题时可以附上这份报告，方便定位。反馈渠道见[社区与反馈](/MirageWallpaper/reference/community/)。

## 相关设置的位置

- API Key 与端点：[通用与音频设置](/MirageWallpaper/settings/general/)
- 登录与 SteamCMD：[SteamCMD 与 Steam 登录](/MirageWallpaper/workshop/steamcmd/)
- 数据与缓存位置：[数据目录](/MirageWallpaper/advanced/data-directories/)
