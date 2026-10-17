---
title: 架构总览
description: MirageLinuxDisplay 的目标架构、模块划分、数据流与协议不变量。
---

## 目标与边界

核心实现使用 C++20 的资源所有权与容器设施，但安装的头文件保持 C ABI。公共 DTO 采用显式八字节布局，跨边界的 FD、超时与状态计数一律使用定宽类型；线上协议为 `mirage-display-v1.3`，不会因实现语言迁移而改变消息格式，旧次版本端点会被拒绝。

MirageWallpaper 的 Linux 版本由"直接占有 X11 桌面"改为"协议驱动的离屏渲染"：渲染器导出 DMA-BUF 帧，桌面环境集成负责在 DE 自有的壁纸表面上显示，并把指针输入回传给渲染器。

X11 仍然受支持，但本仓库既不会创建、也不会管理裸的 X11 桌面窗口。Plasma X11、Plasma Wayland 等会话变体由各自的 DE 适配器通过 DE 提供的接口处理。

MirageQt 与渲染器之间的 JSON 标准输入通道继续负责暂停、音量、帧率与用户属性等壁纸控制，不属于本显示协议的一部分。

## 目标架构

```text
SceneWallpaper / WebWallpaper / VideoWallpaper 生产者
                         |
                 DMA-BUF + 显式同步
                         |
                         v
             嵌入式 md_broker 路由核心（MirageQt 内）
                         |
          $XDG_RUNTIME_DIR/mirage-wallpaper/display-v1.sock
                         |
                         v
                 libmirage-display
             /            |             \
       KDE Plasma     GNOME Shell     layer-shell
       X11/Wayland     X11/Wayland    Wayland only
```

broker 只转发协议报文与文件描述符。像素数据留在 GPU 显存，从不经过 broker 拷贝。一个 broker 就能提供稳定的发现、多输出路由，以及在渲染器或桌面壳层重启后的自动恢复；稳定输出标识取代了过去的屏幕索引约定。

## 模块划分

| 模块 | 职责 |
|---|---|
| `src/codec` | 报文编解码与 `SCM_RIGHTS` 传输（magic、头校验、FD 精确计数） |
| `src/protocol` | 各消息的编解码与 UTF-8 校验（golden 向量与解析边界） |
| `src/display` | 消费端库：握手、缓冲池、帧、显式同步、指针与窗口状态上报 |
| `src/producer` | 生产端库：握手、缓冲出借、帧提交、同步对象管理 |
| `src/broker` | 路由核心：同 UID 校验、输出路由、格式协商、多消费者扇出 |
| `src/sync` | DRM syncobj 扇出、释放信号与 sync_file 桥接 |
| `src/common` | 内部共享设施：工具、Unix 网络、出站队列、公共握手、DRM ABI |
| `gpu/egl` | `EGL_EXT_image_dma_buf_import` 导入与原生 fence 同步 |
| `gpu/vulkan` | 外部内存 FD 导入、DRM 修饰符枚举、relay/blit 回退与导出 |
| `adapters/kde` | Plasma 6 壁纸插件：Qt Quick 显示项（OpenGL/EGL、Vulkan） |
| `examples` | headless 消费者与 mock broker，用于协议仿真与调试 |
| `tests` | 编解码、协议、会话、路由、生产者、同步与 GPU 单元测试 |

消费者与生产者的连接、握手状态机与出站队列在 `src/common` 中共享，两个角色只保留注册报文的差异，避免出现三份近似的实现。

## 数据流

1. 显示端（DE 适配器）连接 broker，用 `REGISTER_OUTPUT` 注册稳定输出标识、逻辑桌面坐标和 DRM render node，随后上报 `CONSUMER_CAPS`（格式、修饰符、UUID、输入能力）。
2. 渲染端连接 broker，用 `REGISTER_PRODUCER` 上报输出标识、DRM 渲染节点与支持的 `(fourcc, plane_count, modifier)` 元组。
3. broker 为同一稳定标识建立路由，取格式交集协商，向生产者下发带消费者 GPU 身份的 `OUTPUT_CONFIG`。
4. 生产者按 `OUTPUT_CONFIG` 创建 GPU 资源，发送 `PRODUCER_GPU_BOUND` 确认身份；broker 确认一致后才接受带代际编号的 `OFFER_BUFFERS`（DMA-BUF FD 随报文送达）。
5. broker 向已绑定且兼容的每个显示端转发 `BIND_BUFFERS`；帧提交经 `PRODUCER_FRAME` → `FRAME_READY` 转发，携带 acquire sync_file 与 release syncobj，多显示端时用 syncobj 扇出保证每个消费者独立释放。
6. 显示端采样完成后置位 release syncobj；全部显示端解绑（`UNBIND` → `UNBIND_DONE`）后，broker 才允许生产者退役该代际并创建新代际。
7. 显示端通过 `POINTER_*` 与 `WINDOW_STATE` 回传输入与窗口事实，broker 以对应的 producer 侧报文转发给渲染器。

## 协议不变量

- 缓冲池代际编号在同一连接内不重复；一个缓冲槽最多只有一帧在途。
- 没有可用释放槽时，生产者跳过该渲染帧。
- 池拆除顺序固定为 `UNBIND → UNBIND_DONE → 描述符关闭`；替换池绝不复用旧代际。
- 回调负载默认是借用的；显式标注的所有权字段（帧的 acquire/release FD）转移归属；断连时库持有的所有描述符都会恰好关闭一次。
- 指针坐标使用输出物理像素、左上角原点；时间戳使用单调微秒时钟。

## 实现状态

已实现：

- 协议冻结（XML、编解码、golden 向量与畸形报文测试）
- broker 路由核心（含多消费者扇出、重连与池代际管理）
- 消费端与生产端库（含延迟解绑、指针/窗口状态）
- EGL 导入与原生 fence 同步；Vulkan 多平面导入、relay/blit 回退与导出
- KDE Plasma 适配器（OpenGL/EGL 与 Vulkan 双后端，X11/Wayland 通用）

规划中（仓库内尚无实现）：

- GNOME Shell 扩展 + 原生纹理导入助手
- layer-shell 通用消费者（优先 wlroots，其次 Hyprland/Niri）
- 移除 MirageWallpaper 对直接 X11 的所有权（依赖上游渲染器接入）

## 相关

- [协议参考](/protocol/overview/)：线上报文格式细节。
- [适配器边界](/adapters/boundary/)：DE 集成点的职责划分。
- [构建与测试](/guides/build/)：把核心库与 KDE 壁纸包跑起来。
