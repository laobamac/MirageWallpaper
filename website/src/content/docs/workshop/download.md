---
title: 下载与管理
description: 从创意工坊并发下载壁纸、查看实时速度、取消任务，并管理已下载内容。
---

## 开始下载

在作品卡片或详情页发起下载。Mirage 使用已登录的 Steam 会话解析作品内容清单，并将文件下载到自己的壁纸目录。下载需要一个拥有 Wallpaper Engine 的 Steam 账号，不需要 Steam Web API Key。

下载服务直接调用 SteamKit2 的清单和 CDN API。其服务器选择、分块调度、校验与断点复用设计参考 DepotDownloader，但实际任务由 Mirage 自己的服务执行，不依赖 DepotDownloader 命令行程序。

## 实时进度

下载管理器显示：

- 已接收字节与总压缩大小
- 完成百分比
- 最近数秒平滑后的实时下载速度
- 预计剩余时间
- 解析、下载、校验和完成状态

速度来自 Steam CDN HTTP 响应流的实际接收字节，不是磁盘写入量或作品页面标注大小。

## 并发与取消

Mirage 最多同时处理三个作品，全局限制为八个分块请求。单个作品下载时可使用全部可用分块连接；多个作品同时下载时共享该上限。每个作品拥有独立取消令牌；取消一个任务不会终止 Steam 会话或其他下载。

Steam 连接短暂中断时，服务会保留下载暂存内容并自动恢复会话和下载任务。只有 Steam 明确拒绝保存的会话时，才需要重新登录。

## 校验与安装

文件先写入 `Workshop/content/431960/.staging`。Mirage 会校验清单中的每个分块并确认根目录存在 `project.json`，然后在同一磁盘卷上原子替换正式作品目录。失败或取消时不会把半成品加入壁纸库，保留的暂存分块可供下次重试复用。

## 更新与重新下载

重新下载已安装作品时，Mirage 会先校验现有文件，只请求缺失或变化的分块。完成后作品仍位于同一目录，壁纸库会自动刷新。

## 批量下载以前已订阅的壁纸

Mirage 的内置下载器按作品 ID 处理任务，目前不会读取并批量导入账号的完整订阅列表。如果你以前已经在 Wallpaper Engine 创意工坊订阅了大量壁纸，可以选择独立运行 Valve 提供的 [SteamCMD](https://developer.valvesoftware.com/wiki/SteamCMD)，让 Steam 一次拉取该账号的 Wallpaper Engine 本体和已订阅内容。

SteamCMD 不是 Mirage 的组件，也不会被 Mirage 安装、启动或管理。下面的流程完全在 Mirage 之外运行。

### 准备条件

- 使用拥有 Wallpaper Engine 且已经订阅目标壁纸的 Steam 账号。
- 预留足够空间存放 Windows 版 Wallpaper Engine 和全部订阅内容；实际大小取决于订阅数量。

### 安装独立 SteamCMD

可以通过 Homebrew 安装：

```bash
brew install --cask steamcmd
```

也可以从 Valve 官方地址手动安装：

```bash
mkdir -p "$HOME/SteamCMD"
cd "$HOME/SteamCMD"
curl -fsSL "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_osx.tar.gz" | tar -xz
```

### 登录并拉取订阅内容

使用一个固定目录启动 SteamCMD。Homebrew 版本执行：

```bash
steamcmd +force_install_dir "$HOME/Steam/WallpaperEngine"
```

手动安装版本执行：

```bash
"$HOME/SteamCMD/steamcmd.sh" +force_install_dir "$HOME/Steam/WallpaperEngine"
```

进入交互控制台后依次执行：

```ansi
Steam> login 你的账户名
Steam> @sSteamCmdForcePlatformType windows
Steam> app_update 431960 validate
Steam> quit
```

登录命令会安全地交互请求密码。账号启用 Steam Guard 时，按提示输入验证码或在 Steam 手机应用中确认。`app_update` 会下载 Wallpaper Engine 本体及该账号已经订阅的创意工坊内容；订阅越多，耗时和占用空间越大。

### 添加到 Mirage

下载完成后，作品通常位于：

```text
~/Steam/WallpaperEngine/steamapps/workshop/content/431960/
```

打开 **Mirage 设置 → 通用**，在壁纸目录中为“Steam 创意工坊目录”点击“选择目录…”，选择上面的 `431960` 目录。Mirage 刷新壁纸库后，这些作品会出现在“已安装”中。

以后新增订阅或需要更新时，重新运行 SteamCMD 并执行 `app_update 431960 validate`，然后刷新 Mirage 壁纸库即可。Mirage 自己的创意工坊下载仍使用内置 SteamKit 服务，与这个外部目录相互独立。

## 预设及其依赖

预设依赖一个基础壁纸。缺少依赖时，Mirage 会先征求同意并把基础作品加入下载队列，完成后再应用预设。详见[预设与依赖](/workshop/presets/)。

实际路径见[数据目录](/advanced/data-directories/)。
