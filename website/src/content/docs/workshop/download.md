---
title: 下载与管理
description: 从创意工坊下载壁纸、查看下载进度、取消任务，并管理已下载内容。
---

在创意工坊里找到心仪的壁纸后，Mirage 会用托管的 SteamCMD 把它下载到本地壁纸库。

## 开始下载

在物品卡片或详情页发起下载。Mirage 会通过已登录的 SteamCMD 会话下载对应的创意工坊物品（App ID `431960`）到隔离的下载目录。

下载需要先完成 [SteamCMD 与 Steam 登录](/workshop/steamcmd/)。若尚未登录或会话失效，Mirage 会提示你先完成设置。

## 下载进度与角标

进行中的下载会显示状态。顶部「创意工坊」标签会出现一个**角标**，显示当前活动下载的数量，方便你随时掌握进度。

## 并行与取消

Mirage 支持多个下载任务。你可以随时**取消**某个正在进行的下载；已取消的任务会被标记，不会污染壁纸库。

## 下载完成后

下载完成的壁纸会进入你的本地壁纸库，可在「已安装」标签中浏览、筛选和应用。它们归入创意工坊来源，实际路径见[数据目录](/advanced/data-directories/)。

## 预设及其依赖

如果你下载的是**预设**，它需要一个底层作品才能正常显示。当你尝试应用一个缺少依赖的预设时，Mirage 会弹出提示并帮你先下载所需的依赖作品，依赖就绪后再应用预设。详见[预设](/workshop/presets/)。

## 更新与重新下载

创意工坊内容可能有更新。重新下载同一物品会让 SteamCMD 拉取最新版本。你也可以在壁纸库中删除不再需要的下载来释放空间。

## 批量拉取已订阅壁纸

如果你在 Wallpaper Engine 中已经订阅了大量壁纸，可以通过 SteamCMD 一次性批量拉取所有已订阅内容，无需逐个搜索下载。

### 前提条件

- 拥有一个已在 Wallpaper Engine 创意工坊中订阅了壁纸的 **Steam 账号**
- 本流程会下载 Wallpaper Engine 本体（约 1 GB），同时连带拉取所有已订阅的创意工坊内容

### 第一步：安装 Steam 客户端并登录

前往 [Steam 官网](https://store.steampowered.com/) 下载安装 macOS 版 Steam 客户端，登录你已订阅壁纸的 Steam 账号。这一步确保你的订阅记录与账号绑定，后续 steamcmd 才能识别。

### 第二步：安装 steamcmd

:::tip
Mirage 首次使用创意工坊时会自动下载并安装 steamcmd。但若你想**独立管理一个专用的 steamcmd 实例**来区分壁纸来源，建议按以下方式手动安装。
:::

**方式一：Homebrew 安装**

```bash
brew install steamcmd
```

**方式二：手动下载**

```bash
mkdir -p ~/Steam && cd ~/Steam
curl -sqL "https://steamcdn-a.akamaihd.net/client/installer/steamcmd_osx.tar.gz" | tar zxvf -

# 添加到环境变量
echo 'export PATH="$PATH:$HOME/Steam"' >> ~/.zshrc
source ~/.zshrc
```

### 第三步：登录 steamcmd 并拉取

```bash
steamcmd
```

进入交互控制台后，依次执行：

```ansi
Steam> login 你的用户名 你的密码
```

如果账号开启了 Steam Guard：

- **验证码方式**：在命令后追加验证码 `login 用户名 密码 验证码`
- **手机确认方式**：不填验证码，在 Steam 手机 App 上批准登录即可

登录成功后，设置平台类型并下载：

```ansi
Steam> @sSteamCmdForcePlatformType windows
Steam> app_update 431960 validate
```

:::caution
下载过程可能较长——steamcmd 会同时拉取 Wallpaper Engine 本体和你**所有已订阅的创意工坊壁纸**，具体耗时取决于订阅数量和网络状况。
:::

下载完成后输入 `quit` 退出 steamcmd。

### 第四步：打开 Mirage

启动 Mirage，壁纸库会自动扫描 steamcmd 下载目录中的创意工坊内容。你已订阅的所有壁纸将出现在「**已安装**」标签中，可直接应用。

若壁纸未出现：
- 确认 steamcmd 下载目录正确。Mirage 默认扫描的路径见[数据目录](/advanced/data-directories/)
- 前往 **Mirage 设置 → 通用**，检查或修改「steamcmd 创意工坊路径」

### 后续维护

- **日常使用**：Mirage 内置的创意工坊浏览 / 下载功能可正常使用，与手动拉取互不冲突
- **更新壁纸**：通过 Steam 客户端可自动更新已订阅内容；也可在 steamcmd 中重新执行 `app_update 431960 validate` 拉取最新版本
- **新增订阅**：在 Steam 客户端（或 Wallpaper Engine）中订阅新壁纸后，重新执行第三步即可同步到 Mirage

## 遇到问题

下载失败、卡住或登录异常时，先检查网络与 Steam 登录状态，再参考[故障排查](/workshop/troubleshooting/)。Mirage 提供脱敏的诊断报告可用于反馈。
