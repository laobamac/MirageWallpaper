---
title: 缓冲池与帧
description: BIND_BUFFERS、SET_CONFIG、FRAME_READY 与池替换（UNBIND/UNBIND_DONE）的报文与所有权规则。
---

## 绑定缓冲池

只有生产者与消费者协商出兼容格式、且生产者已确认 `OUTPUT_CONFIG` 中的目标
GPU 后，broker 才会发送 `BIND_BUFFERS`：

```text
u64 generation
u32 buffer_count
u32 width
u32 height
u32 fourcc
u32 plane_count
u64 modifier
array<plane_desc> descriptors
```

平面描述符按"缓冲为主、平面为次"排序：

```text
u32 stride
u32 offset
u64 size
```

描述符数量必须等于 `buffer_count * plane_count`，附带的 FD 数也必须相同。版本 1 允许 2–4 个缓冲、1–4 个平面。消费库在解绑或断连之前一直持有这些描述符；回调只借用。

`SET_CONFIG`：

```text
u64 config_generation
rect source
rect destination
u32 transform
f32 clear_r
f32 clear_g
f32 clear_b
f32 clear_a
```

## 帧与同步

`FRAME_READY`：

```text
u64 buffer_generation
u32 buffer_index
u32 reserved
u64 sequence
```

恰好携带两个 FD：

1. acquire `sync_file`：生产者在写完帧后将其置位。
2. 二进制 release DRM syncobj FD：初始处于未置位状态。

两个描述符的所有权转移给帧回调。消费者在采样前等待 acquire 描述符，在最后一次 GPU 读取后置位 release syncobj。关闭尚未置位的 release 描述符属于异常回退，可能导致生产者在该槽位上超时。

对于 Vulkan 对端，版本 1 固定跨进程图像状态为 `VK_IMAGE_LAYOUT_GENERAL`。发布帧前，生产者把队列族所有权释放到 `VK_QUEUE_FAMILY_FOREIGN_EXT`；Vulkan 消费者在首次读取前从 `VK_QUEUE_FAMILY_FOREIGN_EXT` 获取，并在置位 release 信号量之前释放回该族。这些是协议不变量，不会在每帧报文中重复。

非当前绑定代际的帧会被直接关闭并丢弃，不会触发帧回调。

## 池替换

broker 发送 `UNBIND { u64 generation }`，消费者按顺序执行：

1. 停止对该池调度新的读取。
2. 等待或回收全部宿主机 GPU 引用。
3. 调用释放回调。
4. 关闭库持有的全部池 FD。
5. 发送 `UNBIND_DONE { u64 generation }`。

C 消费 API 默认在回调返回后同步完成第 3–5 步。基于渲染线程的适配器可以在释放回调中调用 `md_display_defer_unbind()`，异步销毁 EGL/Vulkan/Qt Quick 引用，再于协议事件线程调用 `md_display_finish_unbind()`；库在显式完成之前一直持有池及其 FD。线上顺序与生产者所有权规则不变。

新 `BIND_BUFFERS` 使用不同的代际。池拆除顺序固定为 `UNBIND → UNBIND_DONE → 描述符关闭`，替换池绝不复用旧代际；同一连接内代际编号不重复，一个缓冲槽最多只有一帧在途。
