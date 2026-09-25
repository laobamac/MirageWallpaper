---
title: 示例
description: headless 消费者与 mock broker 的用法，用于协议仿真与调试。
---

核心库构建后会生成两个示例（`MIRAGE_DISPLAY_BUILD_EXAMPLES=ON`，默认开启）。它们链接 `mirage_display_static`，用 `-Wall -Wextra -Wpedantic -Werror` 编译。

## `mirage_headless_consumer`

无头消费端：以命令行参数连接 broker 的 Unix 域套接字，注册一个固定输出，然后打印缓冲池、配置与帧回调（generation、尺寸、fourcc、modifier、sequence 与同步 FD）。在没有桌面环境的情况下，可以用它验证生产者 → broker → 消费者全链路。

```sh
./build/examples/mirage_headless_consumer \
  /run/user/1000/mirage-wallpaper/display-v1.sock
```

## `mirage_mock_broker`

mock broker：不依赖真实路由核心，直接用 `src/codec.hpp` 与 `src/protocol.hpp` 模拟 `mirage-display-v1` 服务端（`WELCOME`、`BIND_BUFFERS`、`FRAME_READY` 等），用于协议仿真与测试消费端行为。

## 相关

- [测试](/dev/tests/)
- [消费库](/dev/consumer/)
