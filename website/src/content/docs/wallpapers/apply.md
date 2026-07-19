---
title: 应用与预览
description: 选中壁纸、预览效果，并把它设为桌面壁纸。
---

在壁纸库中选中一个作品后，就能预览并把它应用到桌面。

## 预览与选中

点击壁纸卡片将其选中，Mirage 会加载它的预览和属性面板。选中状态下你可以调整[播放参数与自定义属性](/MirageWallpaper/wallpapers/playback/)，再决定是否正式应用。

## 应用到桌面

把选中的壁纸设为当前桌面壁纸后，对应的渲染进程会启动并接管桌面：

- **场景壁纸**由 SceneWallpaper（Vulkan / MoltenVK）渲染。
- **网页壁纸**由 WebWallpaper（WKWebView）渲染。
- **视频壁纸**由 VideoWallpaper（AVFoundation）渲染。

Mirage 会记住当前壁纸，并在应用时同步设置一张**桌面占位图**，这样在渲染进程未运行时（例如登录早期）桌面也不会突兀地变空。

## 覆盖到所有显示器

默认情况下壁纸应用在主显示器。若要让当前壁纸铺满所有显示器，可使用**覆盖到所有显示器**（主窗口或[菜单栏](/MirageWallpaper/wallpapers/menubar/)均可触发）。想为不同显示器分别指定壁纸，见[多显示器](/MirageWallpaper/wallpapers/displays/)。

## 停止壁纸

**停止壁纸**会关闭所有渲染进程并清除当前壁纸。之后桌面回到静态状态，直到你再次应用壁纸。

## 网页壁纸的安全确认

应用未受信任来源的**网页壁纸**时，Mirage 会先弹出安全提示，因为网页壁纸会执行 JavaScript。请仅信任来源可靠的网页壁纸。详见[网页壁纸安全](/MirageWallpaper/settings/web-safety/)。

## 关于不支持的壁纸

如果作品类型无法识别或不受支持，Mirage 不会启动渲染进程。场景格式的兼容性仍在完善，复杂的 Wallpaper Engine 场景可能表现存在差异。
