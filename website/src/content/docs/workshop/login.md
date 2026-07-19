---
title: Steam 登录
description: 在 Mirage 中登录 Steam 账号、处理 Steam Guard，以及复用已保存会话。
---

下载创意工坊内容需要登录一个 Steam 账号。登录在[设置向导](/MirageWallpaper/workshop/setup-wizard/)的第 3 步完成，之后由 SteamCMD 维护会话。

## 用密码登录

1. 输入 **Steam 用户名和密码**。
2. Mirage 把凭据交给本机的 SteamCMD 进程完成登录，**不会把密码上传到任何第三方服务器**，也不会明文长期保存密码。
3. 登录过程的日志会实时显示，出错时可据此判断原因。

登录成功后，SteamCMD 会在隔离目录写入自己的会话配置，Mirage 据此判断已登录，并在后续下载时自动复用。

## Steam Guard

如果账号开启了 Steam Guard，登录时会要求二次验证：

- **验证码方式**：输入邮箱或令牌验证码后继续。若会话已结束，会提示重新登录。
- **手机确认方式**：Mirage 进入等待状态，并在日志中每 5 秒提示「仍在等待」。请在 Steam 手机 App 上点确认，随后自动继续。

## 复用已保存会话

如果本机已有**验证有效的 SteamCMD 会话**，向导会提供「使用已保存会话」：

- 无需再次输入密码即可继续。
- Mirage 只持久化你的**用户名**用于复用，会话本身由 SteamCMD 维护。

若保存的会话已失效，Mirage 会提示「保存的 Steam 会话已失效，请使用密码重新登录」。

## 取消与重试

- 登录过程中可随时**取消**，Mirage 会清空密码和验证码输入。
- 返回上一步也会安全地取消正在进行的登录。
- 遇到问题时可重试，或参考[故障排查](/MirageWallpaper/workshop/troubleshooting/)。

## 隐私

- 密码只用于本机 SteamCMD 登录。
- 诊断日志会对密码、API Key、令牌等敏感字段做脱敏（替换为 `[已隐藏]`）。
- 更多细节见 [SteamCMD 与 Steam 登录](/MirageWallpaper/workshop/steamcmd/)。
