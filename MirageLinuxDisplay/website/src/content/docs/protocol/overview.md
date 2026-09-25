---
title: 协议概览
description: mirage-display-v1.3 的传输、报文头、基本编码、角色与特性位。
---

状态：v1.3 已冻结。`protocol/mirage_display_v1.xml` 是权威定义，本文档是它的可读形式。broker 与适配器必须精确使用 v1.3；旧次版本端点会被拒绝。

## 传输

broker 监听：

```text
$XDG_RUNTIME_DIR/mirage-wallpaper/display-v1.sock
```

套接字为 Linux `AF_UNIX`、`SOCK_SEQPACKET`，权限 `0600`。每条协议消息恰好是一个有序包，文件描述符通过 `SCM_RIGHTS` 随包附带。broker 会拒绝 `SO_PEERCRED.uid` 与自身 uid 不同的对端。

实现可以用 `@` 前缀的 Linux 抽象套接字来支持 socket activation 或测试；生产环境始终使用上述文件系统套接字作为发现路径。

## 报文头

所有字段为小端。报文头恰好 24 字节。

| 偏移 | 类型 | 字段 | 值 |
|---:|---|---|---|
| 0 | `u32` | magic | `0x3150444d`，即字节 `MDP1` |
| 4 | `u16` | major | `1` |
| 6 | `u16` | minor | 协商后的次版本；HELLO 期间为 `0` |
| 8 | `u16` | opcode | 消息操作码 |
| 10 | `u16` | flags | 包标志 |
| 12 | `u32` | payload_size | 头之后的负载字节数 |
| 16 | `u16` | fd_count | `SCM_RIGHTS` 附带描述符数 |
| 18 | `u16` | reserved | 必须为零 |
| 20 | `u32` | serial | 每发送方单调递增 |

最大包尺寸为 65536 字节，因此最大负载为 65512 字节。

标志位 0 为 `OPTIONAL`：只有带该位时，对端才可以忽略未知操作码；未知的必选操作码属于致命协议错误。

## 基本编码

- `u16`、`u32`、`u64`：小端无符号整数。
- `f32`：IEEE-754 binary32，按小端 `u32` 位表示。
- `string`：`u32` 字节长度后跟 UTF-8 字节，不带 NUL 结尾。
- `bytes16`：恰好十六字节。
- `array<T>`：`u32` 数量后跟对应数量的编码值。
- `rect`：四个 `f32`：`x`、`y`、`width`、`height`。

字符串上限 4096 字节；数组受消息自身的数量上限与包尺寸上限约束；解码器会拒绝带尾随字节的包。

## 角色

`HELLO.role` 取值为：

| 值 | 角色 |
|---:|---|
| 1 | 显示消费者 |
| 2 | 渲染生产者 |

broker 是这两种角色的服务端。

## 特性位

| 位 | 名称 | 含义 |
|---:|---|---|
| 0 | explicit sync | acquire sync_file 与 release syncobj 帧描述符 |
| 1 | DRM modifiers | 非线性 DMA-BUF 修饰符协商 |
| 2 | multiplane | 每图像多于一个 DMA-BUF 平面 |
| 3 | pointer axis | 水平与垂直滚动 |
| 4 | window state | 可上报遮盖窗口事实 |
| 5 | color metadata | 保留给后续次版本 |
| 6 | target GPU binding | broker 在 `OUTPUT_CONFIG` 中下发 consumer 的目标 GPU |

`explicit sync` 是版本 1 的必选特性；其余特性按交集协商。v1.3 端点必须在
`HELLO` 中把 `min_minor` 与 `max_minor` 都设为 `3`。当前实现不提供旧次版本
降级连接。

## 相关

- [握手与注册](/protocol/handshake/)
- [缓冲池与帧](/protocol/buffers/)
- [指针输入与窗口状态](/protocol/input/)
- [错误处理与生产者会话](/protocol/errors/)
