---
title: 错误处理与生产者会话
description: ERROR 报文、致命错误语义、断连清理，以及 OFFER_BUFFERS/PRODUCER_FRAME/RETIRE 等生产者侧消息。
---

## 错误处理

`ERROR` 包含 `u32 code`、`u32 fatal` 与 UTF-8 消息。致命错误在送达后终止会话；协议违规总是致命。

断连时，对端关闭自己持有的全部描述符。无效或过期包里收到的任何描述符，都不能逃过其错误处理路径。

## 生产者会话

渲染端完成公共握手后发送 `REGISTER_PRODUCER`，向 broker 广播自己的稳定输出标识、渲染器类型、DRM 渲染节点、设备与驱动 UUID，以及支持的 `(fourcc, plane_count, modifier)` 元组。

`PRODUCER_ACCEPTED` 之后，broker 发送 `OUTPUT_CONFIG`（选定范围、格式和消费者
目标 GPU）。生产者必须先按该 GPU 身份创建资源并发送 `PRODUCER_GPU_BOUND`；绑定
成功后才可分配新代际并发送 `OFFER_BUFFERS`。每次收到新的 `OUTPUT_CONFIG` 或
重连后都要重新绑定；池 FD 始终归生产者所有，协议库在排队发送时复制它们。

每条 `PRODUCER_FRAME` 携带与显示端 `FRAME_READY` 相同的负载与两个同步 FD；帧 FD 的所有权转移给生产者协议库，broker 向显示端转发等价的描述符。

生产者在源/目标摆放、变换或渲染器自有清屏色变化时发送 `PRODUCER_SET_CONFIG`；broker 原样转发为显示端 `SET_CONFIG`。

收到 `RETIRE_BUFFERS` 时，生产者停止提交该代际，等待本地 GPU 使用结束，销毁池并发送 `RETIRE_DONE`；对退役代际的新帧会被拒绝。

## 池退役流程

1. broker 先向显示端发送 `UNBIND`，再向生产者发送 `RETIRE_BUFFERS { generation }`。
2. 显示端完成 `UNBIND → UNBIND_DONE` 后，broker 才允许生产者退役该代际。
3. 生产者停止提交、等待本地 GPU 使用结束、销毁池并发送 `RETIRE_DONE`。

没有可用释放槽时，生产者跳过该渲染帧；对退役代际的帧提交会被拒绝。
