---
title: 故障排查
description: 构建、连接、GPU 与桌面环境的常见问题与排查路径。
---

## 构建问题

**Vulkan / EGL 助手没有构建？**

助手会在检测到相应开发环境时自动构建。请确认安装了 Vulkan 开发包（`find_package(Vulkan)` 可检测）或 EGL/GLESv2 开发包；也可以用 `-DMIRAGE_DISPLAY_WITH_VULKAN=ON` / `-DMIRAGE_DISPLAY_WITH_EGL=ON` 显式开启。对应依赖缺失时会直接报错，不会静默跳过。

**KDE 壁纸包构建报错？**

`MIRAGE_DISPLAY_PLUGIN_QML=ON` 要求 `MIRAGE_DISPLAY_WITH_EGL=ON`，并且需要 Qt 6.5+（Gui、Qml、Quick）与 EGL/GLESv2 开发包。建议使用独立的 `build-kde-package` 目录，避免复用旧缓存。

## 连接问题

**消费者连不上 broker？**

- 确认 broker 已监听 `$XDG_RUNTIME_DIR/mirage-wallpaper/display-v1.sock`，权限为 `0600`。
- 确认消费者与 broker 属于同一 UID：broker 通过 `SO_PEERCRED` 拒绝不同 UID 的对端。
- 命令行工具可以用 `mirage_mock_broker` 做端到端验证。

**握手失败或连接立即断开？**

检查 `md_display_connect` 返回的错误码与 `on_disconnected` 的 `reason`/`message`。协议违规总是致命错误；同时确认 `mirage-display-v1` 的报文头、FD 数量与消息格式符合[协议参考](/protocol/overview/)。

## 帧与同步问题

**没有帧回调？**

- 确认生产端与消费端协商出了兼容的 `(fourcc, plane_count, modifier)`：broker 取交集协商，不匹配就不会下发 `BIND_BUFFERS`。
- 确认缓冲池已经完成 `BIND_BUFFERS` 且不是旧代际：非当前绑定代际的帧会被直接关闭丢弃。

**同步描述符泄露或重复关闭？**

帧的 acquire sync_file 与 release syncobj FD 的所有权转移给帧回调，每条路径都必须恰好关闭一次；关闭尚未置位的 release 描述符属于异常回退，可能导致生产者在该槽位上超时。详见[缓冲池与帧](/protocol/buffers/)。

**Vulkan 报 "Vulkan DMA-BUF pool import failed"？**

这是 `md_vk_importer_import_pool` 返回失败时显示项上报的错误，意思是当前缓冲池里的 DMA-BUF 无法导入为 Vulkan 图像。从本版本起，错误信息会追加具体的失败阶段与 `VkResult`，例如：

```
Vulkan DMA-BUF pool import failed: image creation failed (VkResult=VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT), fourcc=XBGR modifier=0x0, buffer=0
```

请按错误信息中的阶段依次排查：

1. **先看失败阶段落在哪一步。**
   - `image creation failed`：驱动不接受该（格式、修饰符、布局）组合的显式 tiling 图像。多为驱动限制——NVIDIA 专有驱动对 `VK_EXT_image_drm_format_modifier` 的显式布局校验更严格，部分版本只支持特定修饰符；请核对信息里的 `fourcc` 与 `modifier`（`0x0` 表示 LINEAR）是否确实受支持，必要时在渲染端换用无修饰符/线性组合。
   - `DMA-BUF memory FD properties query failed` / `no memory type accepts this DMA-BUF`：通常是 **PRIME 双显卡**或驱动只通过非显存（非 `DEVICE_LOCAL`）类型暴露 DMA-BUF。实现会自动优先选显存类型、失败后逐个尝试其余兼容类型；仍失败时请确认两端 GPU 的驱动都支持 `VK_KHR_external_memory_fd`，且该修饰符在两块 GPU 上同时受支持。若 `VkResult` 为 `VK_ERROR_EXTENSION_NOT_PRESENT`，说明设备未启用 DMA-BUF 导入扩展，见下方 NVIDIA 章节。
   - `DMA-BUF memory import allocation failed (VkResult=VK_ERROR_INVALID_EXTERNAL_HANDLE…)`：驱动拒绝了该 FD 的导入（fd 无效、修饰符不兼容或驱动限制）。`memory candidates tried=N` 表示已尝试的内存类型数量。
   - `frame sync FD import failed`：帧的 acquire sync_file 无法导入为 Vulkan 信号量，通常是 `VK_KHR_external_semaphore_fd` 支持不全。
2. **再确认格式组合是否协商成功。** broker 只会下发消费端与生产端都支持的 `(fourcc, plane_count, modifier)` 组合。`md_vk_query_format_caps` 现在会用外部内存能力查询过滤掉无法导入的修饰符，避免上报自身导入不了的组合；如果协商结果仍不可导入，请核对两端 GPU 与驱动版本。
3. **最后确认多平面格式的约束。** NV12 这类 disjoint 格式要求 `plane_count == 2`，并且驱动必须支持对应的 `VkSamplerYcbcrConversion`，否则会在创建转换器或绑定平面内存时失败。

**N 卡壁纸黑屏，报错 "DMA-BUF memory FD properties query failed (VK_ERROR_EXTENSION_NOT_PRESENT)"？**

这是 N 卡用户最常见的黑屏原因，**照着下面三步做就能解决（已在 NVIDIA 真机上验证过）**。

先用人话说说原因：壁纸的画面要借一块"共享显存"从渲染端搬到桌面端，N 卡要开两个开关才允许这么搬——一个在显卡驱动里（驱动本身得提供这个能力），一个在桌面程序里（plasmashell 启动时得把能力清单带上）。任何一个没开，画面就传不过去，桌面只剩一片黑。

**第一步：确认显卡驱动开了开关**

N 卡驱动默认不开这个能力，要在开机加载驱动时加一个 `modeset=1` 参数。先看现在开没开：

```sh
cat /sys/module/nvidia_drm/parameters/modeset   # 输出 Y 表示已开启
vulkaninfo | grep external_memory_dma_buf        # 能看到这行表示驱动已提供能力
```

如果输出是 `N`，执行下面两条命令再重启电脑：

```sh
echo "options nvidia-drm modeset=1" | sudo tee /etc/modprobe.d/nvidia-modeset.conf
sudo update-initramfs -u
sudo reboot
```

> 提示：`update-initramfs` 是 Debian/Ubuntu 系的命令；Arch 系用 `sudo mkinitcpio -P`，其他发行版请用对应的重建 initramfs 命令。重启后重新跑一遍上面的检查，确认输出正确再继续。

**第二步：让 plasmashell 带上扩展清单**

壁纸插件跑在 plasmashell（桌面外壳程序）里，而这个程序一启动就把显卡功能清单定死了，壁纸插件没机会往里面加东西。Qt 为此留了个环境变量开关 `QT_VULKAN_DEVICE_EXTENSIONS`，把它写进 systemd 用户环境，plasmashell 每次启动都会自动带上：

```sh
mkdir -p ~/.config/environment.d
cat > ~/.config/environment.d/mirage-wallpaper.conf <<'EOF'
QT_VULKAN_DEVICE_EXTENSIONS="VK_KHR_external_memory;VK_KHR_external_memory_fd;VK_EXT_external_memory_dma_buf;VK_EXT_queue_family_foreign;VK_EXT_image_drm_format_modifier;VK_KHR_external_semaphore;VK_KHR_external_semaphore_fd;VK_KHR_sampler_ycbcr_conversion;VK_KHR_bind_memory2;VK_KHR_get_memory_requirements2"
EOF
```

**第三步：重启桌面外壳**

```sh
systemctl --user restart plasma-plasmashell.service
```

桌面图标和任务栏会闪一下再回来，属正常现象，不用注销重登。

**怎么确认修好了？** 壁纸正常播放动画；在壁纸设置里打开 "Show diagnostics" 能看到渲染后端是 Vulkan、帧数在持续上涨；桌面上不再冒出 `EXTENSION_NOT_PRESENT` 的报错。

如果不想折腾这些，把桌面渲染后端切回 OpenGL 也一样能显示（EGL 传输方式不依赖上面这些 Vulkan 扩展）。

> 小提示：这两个开关都弄好还黑屏的话，壁纸会在启动时直接告诉你卡在哪一步——报 `driver does not expose ...` 说明第一步没做对；报 `scene-graph device did not enable ...` 说明第二步没生效。另外壁纸只检查了画面传输的开关，帧同步的开关（`VK_KHR_external_semaphore_fd`）没开时，画面会晚一点在帧同步阶段报 `frame sync FD import failed`，可参照本章开头的同步描述符条目排查。

## 桌面环境问题

**Plasma 桌面点击/右键失效？**

指针观察必须让 Qt 事件过滤器返回 `false`，这样 Plasma 才能继续接收桌面点击、右键菜单、拖放与滚轮事件。请检查壁纸包的 "Forward pointer events" 配置。

**窗口状态不更新？**

窗口与工作区状态来自 Plasma 任务模型或 KWin 工作区接口（如 `org.kde.taskmanager`），不查询裸 X11 窗口。请确认当前会话类型下工作区数据源可用。

**如何收集更多信息？**

开启壁纸包的 "Show diagnostics"（后端、连接状态、输出与帧计数），抓取 broker/适配器日志后提交 [GitHub Issue](https://github.com/elysia-best/MirageLinuxDisplay/issues/new/choose)，并说明系统版本、会话类型（X11/Wayland）、复现步骤、预期结果与实际现象。

## 文档网站部署

推送 `website/**` 到 `master` 会触发 `.github/workflows/deploy-docs.yml` 构建并发布到 GitHub Pages。首次使用前需在仓库 **Settings → Pages** 把发布源设为 **GitHub Actions**；部署地址为 `https://elysia-best.github.io/MirageLinuxDisplay/`。
