---
title: 握手与注册
description: HELLO/WELCOME、显示端注册、消费能力上报与生产端注册的报文格式。
---

## 公共握手

对端的第一条消息必须是 `HELLO`；broker 回复 `WELCOME` 或致命 `ERROR`。

`HELLO`：

```text
u32 role
u16 reserved (zero)
u16 min_minor
u16 max_minor
u64 features
string name
string version
```

`WELCOME`：

```text
u16 selected_minor
u16 reserved
u64 features
string server_name
string server_version
```

收到 `WELCOME` 之后，显示端发送 `REGISTER_OUTPUT`，渲染端发送 `REGISTER_PRODUCER`；注册被接受之前，不允许任何角色专属流量。

## 显示端注册

`REGISTER_OUTPUT`：

```text
string stable_id
string name
u32 physical_width
u32 physical_height
u32 logical_width
u32 logical_height
i32 logical_x
i32 logical_y
u32 scale_120
u32 refresh_mhz
u32 transform
u32 drm_render_major
u32 drm_render_minor
u64 input_caps
```

`logical_x` 与 `logical_y` 是虚拟桌面中的逻辑坐标，采用小端有符号 32 位整数，
单位为逻辑桌面像素，允许负数。`scale_120` 表示逻辑缩放系数乘以 120；
`transform` 使用 `wl_output.transform` 的数值 0–7。

broker 回复 `OUTPUT_ACCEPTED { u64 output_id }`。显示端随后发送 `CONSUMER_CAPS`：

```text
u64 sync_caps
u64 color_caps
u32 max_width
u32 max_height
bytes16 device_uuid
bytes16 driver_uuid
array<format_cap> formats
```

格式能力为：

```text
u32 fourcc
u32 plane_count
u64 modifier
```

至多允许 256 个格式能力。

显示端可以通过 `UPDATE_OUTPUT` 在会话中更新几何信息（物理/逻辑尺寸、逻辑坐标、
`scale_120`、刷新率、变换），broker 据此决定是否重新协商 `OUTPUT_CONFIG`。

`UPDATE_OUTPUT` 的负载顺序为：

```text
u32 physical_width
u32 physical_height
u32 logical_width
u32 logical_height
i32 logical_x
i32 logical_y
u32 scale_120
u32 refresh_mhz
u32 transform
```

broker 按 `stable_id` 管理输出。首个显示消费者注册时触发 `on_output_added`，
几何字段发生变化时触发 `on_output_updated`，最后一个显示消费者断开时触发
`on_output_removed`。这些回调运行在 broker 派发线程；输出结构体和字符串仅在
回调期间有效，宿主跨线程使用前必须复制。

## 生产端注册

渲染端完成公共握手后发送 `REGISTER_PRODUCER`，向 broker 广播自己的稳定输出标识、渲染器类型、DRM 渲染节点、设备与驱动 UUID，以及支持的 `(fourcc, plane_count, modifier)` 元组。

`PRODUCER_ACCEPTED` 之后，broker 向生产者发送带消费者 GPU 身份的
`OUTPUT_CONFIG`：

```text
u32 physical_width
u32 physical_height
u32 logical_width
u32 logical_height
u32 refresh_mhz
u32 transform
u32 fourcc
u32 plane_count
u64 modifier
u32 target_drm_render_major
u32 target_drm_render_minor
u32 target_gpu_flags
bytes16 target_device_uuid
bytes16 target_driver_uuid
```

render node 标识始终有效；`target_gpu_flags` 中的
`MD_TARGET_GPU_DEVICE_UUID_VALID` / `MD_TARGET_GPU_DRIVER_UUID_VALID` 表示对应
UUID 有效，未置位时对应字段必须为零。生产者应先按此身份创建 Vulkan/EGL/VA-API
资源，再发送 `PRODUCER_GPU_BOUND`（render node、device UUID、driver UUID）。
broker 确认身份匹配后才接受 `OFFER_BUFFERS` 与帧，避免跨 GPU 路由 DMA-BUF。
新的 `OUTPUT_CONFIG` 会使先前的绑定失效，生产者必须为新配置重新确认 GPU。

## 协商规则

broker 为同一稳定输出标识建立路由，取消费者与生产者格式能力的**交集**进行协商：

- fourcc 与 modifier 必须完全匹配；
- plane_count 取双方都支持的值；
- 消费者上报的 `max_width` / `max_height` 约束生产者的输出尺寸。

协商成功后，broker 才会进入 GPU 绑定和[缓冲池绑定](/protocol/buffers/)阶段。
