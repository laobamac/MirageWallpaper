---
title: 自动打包与发布
description: Mirage 的 GitHub Actions 工作流如何构建、签名并发布正式版与测试版。
---

Mirage 通过 GitHub Actions 工作流 [`build-macos.yml`](https://github.com/laobamac/MirageWallpaper/blob/main/.github/workflows/build-macos.yml) 自动构建两种架构的渲染器与主程序，并按触发方式发布到不同更新通道。

## 触发条件

工作流在以下情况运行：

- 推送到 `main` 分支；
- 推送名称以 `v` 开头的标签（例如 `v1.2.0`）；
- 在仓库的 Actions 页面手动运行（`workflow_dispatch`）。

## 构建矩阵

| 架构 | 运行器 |
| --- | --- |
| `x86_64` | `macos-15-intel` |
| `arm64` | `macos-15` |

两种架构分别独立构建，各自完成渲染器 + 主程序打包，并使用彼此独立的 appcast 更新源。

## 构建步骤

每个运行器依次执行：

1. 检出仓库（`fetch-depth: 0`，用于计算递增构建号）；
2. 校验 `MIRAGE_STEAM_WEB_API_KEY` Secret 是否为 32 位十六进制；
3. 通过 Homebrew 安装构建依赖；
4. 依次构建三个渲染器与 Mirage 主程序；
5. 使用 Sparkle Ed25519 私钥签名更新包与 appcast；
6. 发布到对应的 GitHub Release 与更新源。

## 发布通道

| 触发 | GitHub Release | 更新通道 |
| --- | --- | --- |
| 推送 `v*` 标签 | 正式 Release | 稳定源 |
| 推送 `main` | 滚动的 `prerelease` Release | beta 源 |

- App 默认自动检查并下载**正式版**；
- 在 [软件更新设置](/MirageWallpaper/settings/updates/) 中开启「接收测试版更新」后，Sparkle 会同时检查 beta 通道；
- 两个架构各自使用独立 appcast；更新包、appcast 和更新说明均经 Sparkle Ed25519 签名。

发布必须串行执行：两种架构会更新同一组签名更新源，工作流通过 `concurrency` 组避免并发发布冲突。

## 版本号与构建号

工作流为每次构建自动写入完整 Git commit 与由 `git rev-list --count` 生成的递增构建号，因此**不需要手动更新版本号**。

只有构建号更高的 commit 才会被安装，避免把较新的开发构建降级为较旧的 Release。

## 屏保组件同步

App 更新后的下一次启动会检查已安装到 `~/Library/Screen Savers` 的 Mirage 屏保组件；仅当其构建号落后于 App 内置组件时，才会原子替换并重启相关系统屏保服务。详见 [屏保](/MirageWallpaper/screensaver/overview/)。

## 所需 Secrets

首次运行前，在仓库的 **Settings → Secrets and variables → Actions** 中添加：

| Secret | 用途 |
| --- | --- |
| `MIRAGE_STEAM_WEB_API_KEY` | 32 位 Steam Web API Key，写入 App 的 Info.plist |
| `MIRAGE_SPARKLE_PRIVATE_KEY` | Mirage 专用 Sparkle Ed25519 私钥，仅用于签名更新包与 appcast |

如果本机安装了 GitHub CLI：

```bash
gh secret set MIRAGE_STEAM_WEB_API_KEY < .secrets/steam_web_api_key
```

:::danger[私钥安全]
`MIRAGE_SPARKLE_PRIVATE_KEY` 绝不能提交到仓库。应保留在登录钥匙串中的原始密钥，并另存一份离线备份。客户端仅包含可公开的公钥。
:::

## 签名与公证现状

当前工作流使用临时签名，**不包含 Apple Developer ID 签名和公证**。首次安装的用户可能仍需在 macOS Gatekeeper 中手动允许 Mirage；但后续更新的真实性由内置 Ed25519 公钥验证。

:::note[关于内置 Key 的可提取性]
GitHub Secrets 可以避免 Key 出现在仓库和普通构建日志中，但无法让客户端内置 Key 成为真正的秘密：发布后的 App 必须包含它，有能力分析 App 的人仍可以提取。若未来需要不可提取的凭据，应把对应请求放到受控服务端，由服务端持有 Key，而不是依赖客户端混淆。
:::
