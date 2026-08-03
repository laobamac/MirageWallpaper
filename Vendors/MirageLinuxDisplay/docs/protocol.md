# mirage-display-v1 线上协议

状态：首个实现已冻结。

## 传输

broker 监听：

```text
$XDG_RUNTIME_DIR/mirage-wallpaper/display-v1.sock
```

套接字为 Linux `AF_UNIX`、`SOCK_SEQPACKET`、权限 `0600`。每条协议消息恰好
是一个有序包，文件描述符通过 `SCM_RIGHTS` 随包附带。broker 拒绝
`SO_PEERCRED.uid` 与自身 uid 不同的对端。

实现可使用 `@` 前缀的 Linux 抽象套接字用于 socket activation 或测试；生产
发现路径始终为上述文件系统套接字。

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

标志位 0 为 `OPTIONAL`：只有带该位时，对端才可以忽略未知操作码；未知必选
操作码是致命协议错误。

## 基本编码

- `u16`、`u32`、`u64`：小端无符号整数。
- `f32`：IEEE-754 binary32，按小端 `u32` 位表示。
- `string`：`u32` 字节长度后跟 UTF-8 字节，不带 NUL 结尾。
- `bytes16`：恰好十六字节。
- `array<T>`：`u32` 数量后跟对应数量的编码值。
- `rect`：四个 `f32`：`x`、`y`、`width`、`height`。

字符串上限 4096 字节；数组受消息自身上限与包尺寸上限约束；解码器拒绝
尾随字节。

## 角色

`HELLO.role` 取值为：

| 值 | 角色 |
|---:|---|
| 1 | 显示消费者 |
| 2 | 渲染生产者 |

broker 对两个角色都是服务器。

## 特性位

| 位 | 名称 | 含义 |
|---:|---|---|
| 0 | explicit sync | acquire sync_file 与 release syncobj 帧描述符 |
| 1 | DRM modifiers | 非线性 DMA-BUF 修饰符协商 |
| 2 | multiplane | 每图像多于一个 DMA-BUF 平面 |
| 3 | pointer axis | 水平与垂直滚动 |
| 4 | window state | 可上报遮盖窗口事实 |
| 5 | color metadata | 保留给后续次版本 |

版本 1 要求显式同步；其余特性按交集协商。

## 公共握手

对端的第一条消息是 `HELLO`；broker 回复 `WELCOME` 或致命 `ERROR`。

`HELLO`：

```text
u32 role
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

`WELCOME` 之后，显示端发送 `REGISTER_OUTPUT`，渲染端发送
`REGISTER_PRODUCER`；注册被接受前不允许任何角色专属流量。

## 显示端注册

`REGISTER_OUTPUT`：

```text
string stable_id
string name
u32 physical_width
u32 physical_height
u32 logical_width
u32 logical_height
u32 scale_120
u32 refresh_mhz
u32 transform
u32 drm_render_major
u32 drm_render_minor
u64 input_caps
```

`scale_120` 以每逻辑缩放系数 120 为单位；`transform` 使用 `wl_output.transform`
的数值 0–7。

broker 回复 `OUTPUT_ACCEPTED { u64 output_id }`。显示端随后发送
`CONSUMER_CAPS`：

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

显示端可通过 `UPDATE_OUTPUT` 在会话中更新几何信息（物理/逻辑尺寸、
`scale_120`、刷新率、变换），broker 据此决定是否重新协商
`OUTPUT_CONFIG`。

## 缓冲池

仅在生产者与消费者协商出兼容格式后，broker 才发送 `BIND_BUFFERS`：

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

描述符数量必须等于 `buffer_count * plane_count`，附带 FD 数也须相同。
版本 1 允许 2–4 个缓冲、1–4 个平面。消费库在解绑或断连前持有这些
描述符；回调只借用。

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

1. acquire `sync_file`：生产者写入完成后被信号。
2. 二进制 release DRM syncobj FD：初始未信号。

两个描述符的所有权转移给帧回调。消费者在采样前等待 acquire 描述符，
在最后一次 GPU 读取后信号 release syncobj。关闭未信号的 release
描述符属于异常回退，可能让生产者超时该槽位。

对于 Vulkan 对端，版本 1 固定跨进程图像状态为 `VK_IMAGE_LAYOUT_GENERAL`。
发布帧前，生产者将队列族所有权释放到 `VK_QUEUE_FAMILY_FOREIGN_EXT`；
Vulkan 消费者在首次读取前从 `VK_QUEUE_FAMILY_FOREIGN_EXT` 获取，并在信号
release 信号量前释放回该族。这些是协议不变量，不在每帧报文中重复。

非当前绑定代际的帧直接关闭并丢弃，不触发帧回调。

## 池替换

broker 发送 `UNBIND { u64 generation }`，消费者按序执行：

1. 停止调度对池的新读取。
2. 等待或回收全部宿主机 GPU 引用。
3. 调用释放回调。
4. 关闭库持有的全部池 FD。
5. 发送 `UNBIND_DONE { u64 generation }`。

C 消费 API 默认在回调返回后同步完成第 3–5 步。基于渲染线程的适配器可在
释放回调中调用 `md_display_defer_unbind()`，异步销毁 EGL/Vulkan/Qt Quick
引用，再于协议事件线程调用 `md_display_finish_unbind()`；库在显式完成前
一直持有池及其 FD。线上顺序与生产者所有权规则不变。

新 `BIND_BUFFERS` 使用不同的代际。

## 指针输入

坐标为输出物理像素、左上角原点；时间戳使用单调微秒时钟；修饰符在已知时
使用 Linux 输入修饰符位，否则为零。

指针移动：

```text
f32 x
f32 y
u64 timestamp_us
u32 modifiers
```

指针按键：

```text
f32 x
f32 y
u32 button
u32 state
u64 timestamp_us
u32 modifiers
```

按键取 Linux `BTN_LEFT`、`BTN_RIGHT`、`BTN_MIDDLE`、`BTN_SIDE`、
`BTN_EXTRA` 码；`state` 为 0 释放、1 按下。

指针滚轮：

```text
f32 x
f32 y
f32 delta_x
f32 delta_y
u32 source
u64 timestamp_us
u32 modifiers
```

增量是逻辑滚轮刻度；来源为 wheel=0、finger=1、continuous=2。拖拽由
移动事件加按键状态重建。

## 窗口状态

`WINDOW_STATE { u32 flags }` 携带位标志，由适配器从 DE 的工作区/任务模型
计算，绝不来自裸 X11 查询。

## 错误处理

`ERROR` 包含 `u32 code`、`u32 fatal` 与 UTF-8 消息。致命错误在送达后终止
会话；协议违规总是致命。

断连时对端关闭其持有的全部描述符。无效或过期包中收到的任何描述符都不得
逃过其错误路径。

## 生产者会话

渲染端在公共握手后发送 `REGISTER_PRODUCER`，广告稳定输出标识、渲染器
种类、DRM 渲染节点、设备与驱动 UUID，以及其支持的 `(fourcc, plane_count,
modifier)` 元组。

`PRODUCER_ACCEPTED` 之后，broker 发送 `OUTPUT_CONFIG`（选定范围与格式）。
生产者分配新代际并发送 `OFFER_BUFFERS`，为每个缓冲平面附带一个 DMA-BUF
FD；池 FD 始终归生产者所有，协议库在排队发送时复制它们。

每条 `PRODUCER_FRAME` 携带与显示端 `FRAME_READY` 相同的负载与两个同步
FD；帧 FD 所有权转移给生产者协议库，broker 向显示端转发等价的描述符。

生产者在源/目标摆放、变换或渲染器自有清屏色变化时发送
`PRODUCER_SET_CONFIG`；broker 原样转发为显示端 `SET_CONFIG`。

`RETIRE_BUFFERS` 时，生产者停止提交该代际，等待本地 GPU 使用结束，销毁
池并发送 `RETIRE_DONE`；对退役代际的新帧将被拒绝。
