---
title: 生产库（libmirage-display producer）
description: mirage_display_producer.h 的 C ABI 速览：渲染端会话、缓冲出借、帧提交与同步对象管理。
---

生产库供 MirageWallpaper 渲染器（渲染生产者）使用：负责连接 broker、广播输出与格式能力、出借 DMA-BUF 缓冲池并提交帧。头文件 `include/mirage_display_producer.h`。

## 生命周期

```c
md_producer_t* p = md_producer_new(&callbacks);   /* 调用方持有 */
/* ... 连接与派发 ... */
md_producer_free(p);                               /* 调用方释放 */
```

`md_producer_free` 关闭连接，并清理全部持有的描述符。

## 回调

```c
typedef struct md_producer_callbacks {
    void (*on_connected)(void* user_data, uint64_t producer_id, uint64_t output_id);
    void (*on_output_config)(void* user_data, const md_producer_config_t* config);
    void (*on_retire_buffers)(void* user_data, uint64_t generation);
    void (*on_pointer_enter/leave/motion/button/axis)(...);
    void (*on_window_state)(void* user_data, uint32_t flags);
    void (*on_disconnected)(void* user_data, md_result_t reason, const char* message);
    void* user_data;
} md_producer_callbacks_t;
```

`on_output_config` 携带协商后的 `md_producer_config_t`（物理尺寸、刷新率、变换、
`(fourcc, plane_count, modifier)` 以及目标 DRM render node、可选 GPU UUID）。
回调返回后，生产者必须按这些字段创建 GPU 资源，并调用
`md_producer_bind_gpu`；在此之前不能出借池或提交帧。`on_retire_buffers` 通知
生产者退役指定代际。`on_window_state` 携带桌面窗口事实位标志（`WINDOW_STATE`：
0x1 遮盖、0x2 失焦、0x4 最大化、0x8 全屏），在派发线程调用，值仅对该次调用
借用；渲染器可据此暂停/恢复播放。

## 连接与握手

与消费库对称：`md_producer_begin_connect` / `md_producer_begin_connected_fd` / `md_producer_advance_handshake` / `md_producer_connect`（阻塞）。`md_producer_info_t` 包含稳定输出标识、渲染器类型、DRM 渲染节点、设备与驱动 UUID，以及 `(fourcc, plane_count, modifier)` 能力数组（借用）。

## 缓冲出借与帧提交

```c
md_result_t md_producer_bind_gpu(p, &gpu);             /* 必须先于 offer_buffers */
md_result_t md_producer_offer_buffers(p, &pool);       /* 池 FD 借用，内部复制后排队发送 */
md_result_t md_producer_set_config(p, &config);
md_result_t md_producer_submit_frame(p, generation, buffer_index, sequence,
                                     acquire_sync_fd, release_syncobj_fd);
md_result_t md_producer_retire_done(p, generation);
```

- `offer_buffers` 的池 FD 是借用的，库在排队发送时复制它们；池本身始终归生产者所有。
- `md_producer_bind_gpu` 的身份必须与最近一次 `OUTPUT_CONFIG` 一致；新的输出配置或重连后需要再次调用。
- `submit_frame` 会**消费**两个帧 FD（包括错误路径），所有权转移给生产者协议库。
- 没有可用释放槽时跳过该渲染帧；对退役代际的新帧会被拒绝。

## 相关

- [消费者库](/dev/consumer/)
- [错误处理与生产者会话](/protocol/errors/)
- [GPU 助手](/dev/gpu/)（`md_vk_exporter_export_frame` 与 `submit_frame` 配合）
