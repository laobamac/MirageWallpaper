---
title: Steam Web API Key
description: 为什么以及如何配置你自己的 Steam Web API Key，让创意工坊浏览更稳定。
---

Mirage 通过 **Steam Web API** 来浏览、搜索和获取创意工坊物品详情。这一步需要一个 API Key。

## 内置 Key 与自定义 Key

Mirage 附带一个**内置的 Steam Web API Key**，所有用户共享。它开箱即用，但因为大家都在用，高峰期容易触发限流、返回繁忙或变慢。

填写**你自己的 API Key** 后，Mirage 会优先使用它，浏览与搜索会更稳定、更快。Key 需要是标准的 32 位十六进制字符串，Mirage 会校验格式后才启用；格式无效时自动回退到内置 Key。

## 如何获取

1. 用你的 Steam 账号登录 [Steam Web API Key 申请页](https://steamcommunity.com/dev/apikey)。
2. 按提示填写域名（可填任意你拥有或占位的域名），同意条款。
3. 复制生成的 32 位 Key。

## 在 Mirage 中填写

打开[设置](/MirageWallpaper/settings/general/)，找到 Steam 创意工坊相关选项，把 Key 粘贴进去。Mirage 会去除首尾空白并校验格式，有效后立即生效。

## API 端点

Mirage 提供两个端点：

- **官方**：`api.steampowered.com`
- **镜像**：便于在访问官方端点不畅的网络环境下使用

在设置中切换端点。无论走哪个端点，都会使用你配置的（或内置的）API Key。

:::note[隐私]
你的 API Key 保存在本地设置中。Mirage 的诊断日志会对 Key、令牌、密码等字段做脱敏处理（替换为 `[已隐藏]`），不会明文写入日志。请勿把你的 Key 公开分享。
:::

## 常见问题

- **浏览一直繁忙或超时**：多半是内置 Key 被限流，填写自己的 Key 通常能解决。
- **提示 Key 无效**：确认是 32 位十六进制字符串，且没有多余空格。
- **国内访问慢**：尝试切换到镜像端点。

更多排查见[故障排查](/MirageWallpaper/workshop/troubleshooting/)。
