---
title: GPU 助手
description: EGL 与 Vulkan 的 DMA-BUF 导入、relay/blit 回退、帧同步导入与导出。
---

GPU 助手把协议帧（DMA-BUF + 显式同步）接入宿主的 EGL 或 Vulkan 场景，让 DE 适配器在自有壁纸表面上采样。两套助手都提供稳定的 C ABI，DTO 显式打包。

## EGL（`mirage_display_egl.h`）

使用 `EGL_EXT_image_dma_buf_import` 导入，配合原生 fence 同步：

```c
md_egl_importer_t* imp = md_egl_importer_new(&ctx);   /* ctx 只需 EGLDisplay */
md_egl_importer_import_pool(imp, &pool);              /* 生成 EGLImage */
md_egl_wait_acquire_sync(imp, acquire_sync_fd);       /* 消费 acquire FD，插入原生 fence 等待 */
md_egl_release_after_current_context(imp, release_syncobj_fd); /* 消费 release FD */
md_egl_importer_release_pool(imp);                    /* 释放全部 EGLImage */
```

`md_egl_imported_pool_t` 保存每个缓冲的 `EGLImageKHR`；`release_after_current_context` 从当前 GL 上下文把 fence 附加到 release syncobj。

## Vulkan（`mirage_display_vulkan.h`）

external memory FD / DRM 修饰符导入：

- `md_vk_importer_new(&ctx)`：使用已创建的 instance/device，`image_usage` 由调用方指定。
- `md_vk_importer_import_pool`：为每个平面绑定 `VkDeviceMemory`，创建 `VkImage`/`VkImageView`；NV12 等格式还会创建 `VkSamplerYcbcrConversion`。
- `md_vk_import_acquire_sync` / `md_vk_import_release_syncobj`：导入并消费帧同步 FD，返回可提交的信号量（acquire 是临时导入，release 是二进制信号量，必须在最终读提交中置位）。
- `md_vk_importer_acquire_barrier` / `release_barrier`：协议 v1 要求的 `VK_IMAGE_LAYOUT_GENERAL` 队列族所有权屏障。
- `md_vk_fourcc_to_format` / `md_vk_query_format_caps`：fourcc 映射与 DRM 修饰符能力枚举（`caps=NULL, capacity=0` 时只查询数量）。枚举时会用外部内存能力查询过滤掉无法作为 DMA-BUF 导入的修饰符，避免上报自身导入不了的组合。
- `md_vk_importer_last_error` / `md_vk_import_stage_string`：最近一次导入失败的结构化记录与阶段说明，用于在桌面端给出可定位的错误信息。
- `md_vk_query_dma_buf_import_support` / `md_vk_dma_buf_import_state_string`：在创建 importer 之前探测设备能否导入协议 DMA-BUF。探测区分两种失败：驱动不暴露所需扩展（`DRIVER_UNSUPPORTED`，如 NVIDIA 未开启 `nvidia-drm modeset`），以及扩展存在但设备未启用（`DEVICE_NOT_ENABLED`，如 Qt Quick 场景图在插件注册扩展之前就已创建设备）。前者返回缺失扩展名列表，后者通过 `vkGetMemoryFdPropertiesKHR` 的行为探测（对非 DMA-BUF fd，启用时返回 `VK_ERROR_INVALID_EXTERNAL_HANDLE`，未启用时返回 `VK_ERROR_EXTENSION_NOT_PRESENT`）判定。
- `md_vk_result_string`：把 `VkResult` 转成可读字符串。

导入 DMA-BUF 时，`VkDeviceMemory` 必须落在该缓冲区支持的内存类型上。实现优先选择 `DEVICE_LOCAL`（显存）类型；当缓冲区只在非显存类型上可导入——常见于 PRIME 双显卡与部分专有驱动——就退而求其次，把其余兼容类型逐个尝试一遍，直到分配成功。没有这层回退，这些环境会直接导入失败，表现为壁纸播放时报 "Vulkan DMA-BUF pool import failed"；此时壁纸叠加层会显示具体的失败阶段与 `VkResult`，可按[故障排查](/reference/troubleshooting/)逐步定位。

KDE 桌面（plasmashell）下走 Vulkan 渲染后端时，Qt Quick 场景图在壁纸插件有机会注册设备扩展之前就已创建设备，因此 `md_vk_query_dma_buf_import_support` 通常返回 `DEVICE_NOT_ENABLED`。Qt 为这类 Qt Quick 应用提供的启用途径是 `QT_VULKAN_DEVICE_EXTENSIONS` 环境变量（分号分隔的扩展名列表，见[故障排查](/reference/troubleshooting/)）；也可以在启动时探测失败后直接改用 OpenGL 渲染后端走 EGL 导入。

### relay/blit 回退（`mirage_display_vulkan_blit.h`）

对无法直接采样的修饰符，使用同设备 blit 把导入图像复制到宿主可采样的图像：

```c
md_vk_blitter_t* bl = md_vk_blitter_new(&blit_ctx);
md_vk_blitter_blit(bl, pool, buffer_index, acquire_semaphore, release_semaphore);
/* 之后用 md_vk_blitter_image/layout/format/width/height 采样宿主图像 */
```

blit 会等待 acquire 信号量，在导入图像释放回 `VK_QUEUE_FAMILY_FOREIGN_EXT` 后置位 release 信号量，并在返回前等待完成；信号量仍归 importer 所有。

### 导出（`mirage_display_vulkan_export.h`）

渲染端把 Vulkan 图像导出为 DMA-BUF 帧：

```c
md_vk_exporter_t* ex = md_vk_exporter_new(&export_ctx);  /* 可带 drm_render_fd */
md_vk_exporter_create_pool(ex, &info);                   /* 替换当前池 */
md_vk_exporter_acquire(ex, &index);                      /* 轮询 release syncobj，返回空闲槽 */
md_vk_exporter_export_frame(ex, index, semaphore, &acquire_fd, &release_fd);
md_vk_exporter_copy_frame(ex, index, src_img, layout, w, h, &acquire_fd, &release_fd);
md_vk_exporter_cancel_frame(ex, index);                  /* 提交失败后回滚槽位 */
```

- `md_vk_exporter_acquire` 返回 `MD_ERR_WOULD_BLOCK` 表示所有槽位仍被消费者持有；槽位在 release 之前绝不复用。
- `export_frame` 把已置位的二进制信号量导出为 sync_file，并新建处于未置位状态的二进制 DRM syncobj；两个 FD 归调用方所有，用于 `md_producer_submit_frame()`。
- 借用的描述符与 image view 在池替换之前一直有效。

## 相关

- [消费者库](/dev/consumer/)
- [生产者库](/dev/producer/)
- [KDE Plasma 适配器](/adapters/kde/)（EGL 与 Vulkan 双后端）
