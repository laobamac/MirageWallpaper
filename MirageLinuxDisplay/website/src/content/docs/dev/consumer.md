---
title: 消费库（libmirage-display）
description: mirage_display.h 的 C ABI 速览：生命周期、回调、握手、派发、延迟解绑与指针上报。
---

消费库供桌面环境适配器（DE 消费者）使用：连接 broker、注册输出、接收缓冲池与帧、上报指针与窗口状态。头文件 `include/mirage_display.h` 只导出 C11 与 C++ 都能用的稳定 C ABI，公开 DTO 采用显式八字节布局。

## 生命周期

```c
md_display_t* d = md_display_new(&callbacks);   /* 调用方持有 */
/* ... 连接与派发 ... */
md_display_free(d);                              /* 调用方释放 */
```

`md_display_free` 会关闭连接，并把库持有的全部描述符恰好关闭一次。

## 回调

```c
typedef struct md_display_callbacks {
    void (*on_connected)(void* user_data, uint64_t output_id);
    void (*on_buffers_ready)(void* user_data, const md_buffer_pool_t* pool);
    void (*on_buffers_releasing)(void* user_data, const md_buffer_pool_t* pool);
    void (*on_config)(void* user_data, const md_display_config_t* config);
    void (*on_frame)(void* user_data, const md_frame_t* frame);
    void (*on_disconnected)(void* user_data, md_result_t reason, const char* message);
    void* user_data;  /* 借用，库从不释放 */
} md_display_callbacks_t;
```

- 回调负载默认是**借用**的：`pool`、`config` 只在回调内有效。
- `md_frame_t` 的 `acquire_sync_fd` 与 `release_syncobj_fd` **转移所有权**：每条路径都必须恰好关闭一次（采样完成后关闭）。
- 在 `on_buffers_releasing` 中可调用 `md_display_defer_unbind()` 延迟解绑（见下文）。

## 连接与握手

```c
md_result_t md_display_begin_connect(d, socket_path, client_name, client_version,
                                     &output, &caps);
int32_t progress = md_display_advance_handshake(d);
/* MD_HANDSHAKE_* 进度值，或负的 md_result_t */
```

- 非阻塞方式：`begin_connect` 之后循环调用 `advance_handshake`，配合 `md_display_wants_writable()` / `md_display_handle_writable()` 推进写方向。
- `md_display_begin_connected_fd(d, connected_fd, ...)` 接收已连接的 `AF_UNIX SOCK_SEQPACKET` FD（支持 broker 交接、socket activation 与测试），成功后所有权转移给 display。
- `md_display_connect(...)` 是阻塞便捷封装，适合命令行工具与测试。
- 所有输入字符串都是借用的 NUL 结尾字符串，且为必填。

## 派发与事件循环

```c
int32_t md_display_dispatch(d);     /* 返回派发的包数或错误码 */
uint8_t md_display_wants_writable(d);
md_result_t md_display_handle_writable(d);
```

典型的集成方式：把 `md_display_get_fd(d)` 挂到事件源（Qt 的 `QSocketNotifier`、GObject 的 `GSource` 或 `poll`），可读时调用 `dispatch`，`wants_writable` 为真时调用 `handle_writable`。

## 延迟解绑

默认在 `on_buffers_releasing` 返回后**同步**完成解绑。基于渲染线程的适配器需要异步销毁 EGL/Vulkan/Qt Quick 引用：

1. 在 `on_buffers_releasing` 中调用 `md_display_defer_unbind(d)`（只能在回调内调用）。
2. 渲染线程销毁宿主机 GPU 引用。
3. 在协议事件线程调用 `md_display_finish_unbind(d, generation)`。

`md_display_pending_unbind_generation(d)` 返回 0 表示没有待处理的延迟解绑。库在显式完成之前一直持有池及其 FD。

## 指针与窗口状态上报

```c
md_display_send_pointer_enter(d, x, y, timestamp_us);
md_display_send_pointer_leave(d, timestamp_us);
md_display_send_pointer_motion(d, x, y, timestamp_us, modifiers);
md_display_send_pointer_button(d, x, y, button, state, timestamp_us, modifiers);
md_display_send_pointer_axis(d, x, y, delta_x, delta_y, source, timestamp_us, modifiers);
md_display_send_window_state(d, flags);
```

坐标使用输出物理像素、左上角原点；时间戳使用单调微秒时钟。KDE 适配器的指针观察必须让 Qt 事件过滤器返回 `false`，这样 Plasma 才能继续接收桌面点击、右键菜单、拖放与滚轮事件。

## 同步辅助

```c
md_display_signal_release_syncobj(release_syncobj_fd);  /* CPU 回退：置位 release syncobj */
md_display_release_after_sync_file(release_syncobj_fd, sync_file_fd); /* 连接二者 */
```

两个函数都会消费对应 FD，适合没有 GPU 的直接路径。

## 相关

- [生产者库](/dev/producer/)
- [路由核心](/dev/broker/)
- [缓冲池与帧协议](/protocol/buffers/)
