# MirageLinuxDisplay 架构与实现状态

## 目标与边界

MirageWallpaper 的 Linux 版本由"直接占有 X11 桌面"改为"协议驱动的离屏
渲染"：渲染器导出 DMA-BUF 帧，桌面环境集成负责在 DE 自有的壁纸表面上显示，
并把指针输入回传给渲染器。

X11 仍然受支持，但本仓库既不会创建也不会管理裸的 X11 桌面窗口。Plasma X11、
Plasma Wayland 等会话变体由各自的 DE 适配器通过 DE 提供的接口处理。

MirageQt 与渲染器之间的 JSON 标准输入通道继续承担暂停、音量、帧率与用户
属性等壁纸控制，不属于本显示协议的一部分。

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

broker 只转发协议报文与文件描述符。像素数据停留在 GPU 显存，从不经过
broker 拷贝。一个 broker 提供稳定的发现、多输出路由，以及在渲染器或桌面
壳层重启后的自动恢复；稳定的输出标识取代了旧的屏幕索引约定。

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

消费者与生产者的连接、握手状态机与出站队列在 `src/common` 中共享，两个角色
只保留注册报文的差异，避免三份近似的实现。

## 数据流

1. 显示端（DE 适配器）连接 broker，`REGISTER_OUTPUT` 注册稳定输出标识，
   随后上报 `CONSUMER_CAPS`（格式、修饰符、UUID、输入能力）。
2. 渲染端连接 broker，`REGISTER_PRODUCER` 上报输出标识、DRM 渲染节点与
   支持的 `(fourcc, plane_count, modifier)` 元组。
3. broker 为同一稳定标识建立路由，取交集协商格式，向生产者下发
   `OUTPUT_CONFIG`。
4. 生产者创建代际编号的缓冲池并 `OFFER_BUFFERS`（DMA-BUF FD 随报文送达）。
5. broker 向已绑定且兼容的每个显示端转发 `BIND_BUFFERS`；帧提交经
   `PRODUCER_FRAME` -> `FRAME_READY` 转发，携带 acquire sync_file 与
   release syncobj，多显示端时以 syncobj 扇出保证每个消费者独立释放。
6. 显示端采样完成后信号 release syncobj；全部显示端解绑（`UNBIND` ->
   `UNBIND_DONE`）后，broker 才允许生产者退役并创建新代际。
7. 显示端通过 `POINTER_*` 与 `WINDOW_STATE` 回传输入与窗口事实，broker
   以对应 producer 侧报文转发给渲染器。

## 协议不变量

- 缓冲池代际编号在同一连接内不重复；一个缓冲槽最多只有一帧在途。
- 无可用释放槽时，生产者跳过该渲染帧。
- 池拆除顺序固定为 `UNBIND -> UNBIND_DONE -> 描述符关闭`；替换池绝不复用
  旧代际。
- 回调负载默认借用；显式标注的所有权字段（帧的 acquire/release FD）转移
  归属；断连时所有库持有的描述符恰好关闭一次。
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
- layer-shell 通用消费者（wlroots 优先，其次 Hyprland/Niri）
- MirageWallpaper 直接 X11 所有权的移除（依赖上游渲染器接入）

## 验证矩阵

协议层：golden 向量、短包/超长包/尾随字节拒绝、FD 数组缺失/超量/截断、
未知可选/必选 opcode、旧代际丢帧、握手/绑定/帧/解绑各阶段断连、FD 计数
回归。

GPU：Intel/AMD/NVIDIA 驱动、同卡与 PRIME 跨卡、线性与修饰符布局、1x/
分数/混合缩放、尺寸变化、旋转、热插拔与挂起恢复。

桌面环境：Plasma Wayland 与 Plasma X11 全链路；GNOME 与 layer-shell 按
规划推进。
