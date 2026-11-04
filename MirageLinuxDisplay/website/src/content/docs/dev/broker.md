---
title: 路由核心（broker）
description: mirage_display_broker.h 的 C ABI 速览：选项、监听、派发与安全模型。
---

broker 是嵌入式路由核心，运行在 MirageQt 内，独立于 X11 与 Wayland。它持有 Unix 域套接字、校验对端身份、按稳定输出标识匹配生产者与消费者，并转发报文与 FD。头文件 `include/mirage_display_broker.h`。

## 生命周期

```c
md_broker_options_t options = {
    .socket_path = "/run/user/1000/mirage-wallpaper/display-v1.sock",
    .server_name = "mirage",
    .server_version = "0.2.0",
    .features = MD_FEATURE_EXPLICIT_SYNC | MD_FEATURE_DRM_MODIFIERS | ...,
    .max_routes = 128,
};
md_broker_t* b = md_broker_new(&options);   /* 选项字符串会被复制 */
md_result_t r = md_broker_listen(b);        /* 绑定 AF_UNIX SOCK_SEQPACKET 端点 */
/* ... md_broker_dispatch(b, timeout_ms) 事件循环 ... */
md_broker_stop(b);
md_broker_free(b);
```

## 事件循环

`md_broker_dispatch(broker, timeout_ms)` 轮询监听套接字与所有活跃对端；`timeout_ms` 为负数时阻塞直到有事件。它适合放在 MirageQt 的专用事件线程。`md_broker_get_fd` 返回监听 FD，可以挂到外部事件源。

## 安全模型

- 套接字权限 `0600`，只允许同用户访问。
- 通过 `SO_PEERCRED` 校验 `uid` 是否与自身一致，拒绝其他对端。
- broker 只转发协议报文与文件描述符；像素数据留在 GPU 显存，从不经过 broker 拷贝。

## 路由与协商

broker 为同一稳定输出标识建立路由：一个生产者 + 多个 DE 消费者。它取格式能力
交集协商 `(fourcc, plane_count, modifier)`，并把消费者的 DRM render node（以及
可用的设备/驱动 UUID）放入 `OUTPUT_CONFIG`。生产者发送 `PRODUCER_GPU_BOUND` 且
身份匹配后，broker 才下发 `BIND_BUFFERS` 并接受帧；多显示端时用 syncobj 扇出
保证每个消费者独立释放。

### 输出生命周期回调

`md_broker_options_t` 提供 `on_output_added`、`on_output_updated` 和
`on_output_removed` 三个可选回调。它们分别对应首个消费者注册、逻辑几何或
显示参数变化、以及最后一个消费者断开。回调只在 broker 派发线程触发；
`md_output_info_t` 及其字符串是借用数据，不能在回调返回后继续保存。需要更新
Qt 等主线程模型时，应在回调中复制字段，再通过队列信号传递。

## 相关

- [架构总览](/guides/architecture/)
- [协议概览](/protocol/overview/)
- [模拟 broker 示例](/dev/examples/)
