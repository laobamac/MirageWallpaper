---
title: 社区与反馈
description: 报告问题、参与贡献、赞助项目以及了解许可证与鸣谢。
---

Mirage 是一个开源项目，欢迎反馈与参与。它目前仍处于早期阶段，你的问题反馈对完善兼容性很有帮助。

## 报告问题

遇到 Bug 或异常时，请在 GitHub 提交 Issue：

- [提交 Issue](https://github.com/laobamac/MirageWallpaper/issues/new/choose)

为了便于定位，请尽量写清：

- 系统版本与 Mirage 版本；
- 复现步骤；
- 预期结果与实际现象；
- 相关日志（可在[通用设置](/settings/general/)开启「详细日志」，创意工坊问题可导出脱敏的支持报告）。

也可以加入 **QQ 交流群 2160040437** 交流反馈。

## 参与贡献

欢迎提交 Pull Request。提交前请至少确认：

1. 三个渲染器可以独立构建；
2. `./Mirage/scripts/build.sh Release` 能生成完整 App Bundle；
3. App Bundle 中包含三个渲染器、运行时动态库、MoltenVK ICD 和 `assets`；
4. 没有提交 API Key、Steam 登录数据、构建目录或用户壁纸。

构建方法见[从源码构建](/advanced/build/)。

## 赞助

Mirage 会继续免费开放开发。如果它为你的桌面带来了价值，欢迎按自己的意愿赞助——每一份支持都会用于持续维护、兼容性改进和新功能开发。**赞助完全自愿，不影响任何功能使用。**

[支持 Mirage](/reference/support/) 页面直接列出了爱发电、微信支付、支付宝和 USDT，无需跳转到项目 README。

## 许可证

Mirage 使用 [GPL-3.0](https://github.com/laobamac/MirageWallpaper/blob/main/LICENSE) 发布。仓库中的第三方代码与资源继续遵循各自许可证。

## 鸣谢

Mirage 的实现参考并使用了以下项目：

- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) — 运行时 Vulkan → Metal 转译
- [wallpaper-engine-mac](https://github.com/MrWindDog/wallpaper-engine-mac) — UI 框架参考
- [open-wallpaper-engine](https://github.com/waywallen/open-wallpaper-engine) — 粒子系统参考
- [laobamac/OpenMetalWallpaper](https://github.com/laobamac/OpenMetalWallpaper) — 模型解析
- [rstd](https://github.com/hypengw/rstd)

## 关联声明

Mirage 与 Valve、Steam 或 Wallpaper Engine 没有关联，也未获得其官方认可。创意工坊内容归各自作者所有，请遵守相应的许可与使用条款。
